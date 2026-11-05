import QtQuick
import QtQuick.Window
import Pinhole.Plugin

// The whole window, as the map is. At home: the people named so far, then
// the faces not named yet, each group a circle. A group opens to its
// photos with every name a key away; a person to theirs, with the groups
// that look like them to check one after another.
Panel {
    id: panel
    property bool shown: false
    property string source: ""
    property string stateKey: ""
    property var edits: ({})
    // "home", "group" (a suggestion, `anchor` its row), "person" (`person`
    // its name) or "check" (the groups that look like `person`, one by one).
    property string view: "home"
    property var anchor: null
    property string person: ""
    property var people: []
    property var groups: []
    property int groupLimit: 48
    property bool moreGroups: false
    property var faces: []
    property int faceLimit: 96
    property bool moreFaces: false
    property var likely: []
    property int checking: 0
    property int selected: 0
    property var status: ({ phase: "waiting" })
    property string failure: ""
    // The read now wanted (any object: a chain's token) and the program it
    // waits on, which is what can be cancelled.
    property var readJob: null
    property var running: null
    property var assignmentJob: null
    property string picking: ""
    readonly property int columns: Math.max(3, Math.floor((width - 24) / (theme.fontSize * (view === "home" ? 10 : 11))))
    // At home, in this order: named people, faces to name, faces not known
    // (the service sends those last).
    readonly property var known: groups
    property var unknown: []
    // At home: named people, faces not known yet (kept for later), then
    // the faces to name.
    readonly property var tiles: view === "home" ? people.concat(unknown, known) : faces
    readonly property var selection: tiles[selected] ?? null
    readonly property var personRow: people.find(row => row.name === person) ?? null
    readonly property var names: {
        const counts = new Map(people.map(row => [row.name, row.count]))
        for (const key of Object.keys(edits)) {
            for (const name of [edits[key].name, edits[key].person])
                if (name && !counts.has(name)) counts.set(name, 0)
        }
        return Array.from(counts.keys()).sort((a, b) => counts.get(b) - counts.get(a) || a.localeCompare(b))
    }
    // The names a group can be given with a key, its likeliest first.
    readonly property var choices: {
        const guess = view === "group" ? anchor?.guess : view === "check" ? person : null
        const rest = names.filter(name => name !== guess && name !== (view === "person" ? person : null))
        return (guess ? [guess] : []).concat(rest).slice(0, 9)
    }
    readonly property bool working: ["scanning", "grouping", "models"].includes(status.phase)
    visible: shown && !pinhole.away

    function count(n) { return Number(n || 0).toLocaleString(Qt.locale(), "f", 0) }
    function show() {
        if (shown) return
        source = pinhole.library.key || pinhole.library.dir
        stateKey = "people:" + source
        edits = pinhole.state[stateKey] || {}
        shown = true
        view = "home"
        selected = 0
        groupLimit = 48
        status = { phase: "waiting" }
        Qt.callLater(() => grid.forceActiveFocus())
        load()
    }
    function stopReading() {
        running?.cancel()
        running = null
        readJob = null
    }
    function close() {
        stopReading()
        closePicker(false)
        shown = false
        anchor = null
        person = ""
        people = []; groups = []; faces = []; likely = []
        failure = ""
    }
    function request(operation, options) {
        return pinhole.run("pinhole-faces", [operation, "--cache", pinhole.cacheDir], {
            input: JSON.stringify(Object.assign({ source: source, edits: edits, offset: 0 }, options)),
            timeout: 10000
        })
    }
    // One step of the chain `token` is, or nothing once another is wanted.
    function read(token, operation, options) {
        if (readJob !== token) return Promise.reject({ code: "cancelled" })
        const job = request(operation, options)
        running = job
        return job.then(result => {
            if (readJob !== token) throw { code: "cancelled" }
            const response = JSON.parse(result.stdout)
            status = response.status
            failure = ""
            return response.rows
        })
    }
    // Everything the view shows, again: after a change, and every few
    // seconds while the faces are still being found.
    function load() {
        stopReading()
        const kept = selection?.key
        const token = {}
        readJob = token
        // Faces of photos still as they were read, and not hidden from the
        // grid (Screenshots').
        const keep = rows => rows.filter(row => {
            const photo = pinhole.library.item(row.id)
            return photo.thumbPath === pinhole.library.dir + "/" + row.path && !pinhole.isHidden(row.id)
        })
        let job
        if (view === "home") {
            job = read(token, "people", { limit: 500 }).then(rows => {
                people = rows.slice(0, 500)
                return read(token, "groups", { which: "unknown", limit: 500 })
            }).then(rows => {
                unknown = rows.slice(0, 500)
                return read(token, "groups", { which: "known", limit: groupLimit })
            }).then(rows => {
                moreGroups = rows.length > groupLimit
                groups = rows.slice(0, groupLimit)
            })
        } else if (view === "check") {
            job = read(token, "people", { limit: 500 }).then(rows => {
                people = rows.slice(0, 500)
                return read(token, "likely", { person: person, limit: 200 })
            }).then(rows => {
                likely = rows.slice(0, 200)
                checking = Math.min(checking, Math.max(0, likely.length - 1))
                if (!likely.length) return []
                return read(token, "members", { anchor: likely[checking].anchor, limit: 60 })
            }).then(rows => {
                faces = keep(rows.slice(0, 60))
                moreFaces = rows.length > 60
            })
        } else {
            const operation = view === "group" ? "members" : "person"
            job = read(token, "people", { limit: 500 }).then(rows => {
                people = rows.slice(0, 500)
                return read(token, operation, { anchor: anchor?.anchor, person: person, limit: faceLimit })
            }).then(rows => {
                moreFaces = rows.length > faceLimit
                faces = keep(rows.slice(0, faceLimit))
            })
        }
        job.then(() => {
            if (readJob !== token) return
            readJob = null; running = null
            const at = tiles.findIndex(row => row.key === kept)
            selected = at >= 0 ? at : Math.min(selected, Math.max(0, tiles.length - 1))
            if (view === "group" && !faces.length) back()
            if (view === "check" && !likely.length) go("person", person)
        }).catch(error => {
            if (readJob !== token) return
            readJob = null; running = null
            if (error.code !== "cancelled") {
                failure = qsTr("People could not load the face groups.")
                pinhole.warn(error)
            }
        })
    }
    function statusText() {
        switch (status.phase) {
        case "scanning": return qsTr("Looking for faces: %1 of %2 photos so far.").arg(count(status.done)).arg(count(status.total))
        case "grouping": return qsTr("Grouping similar faces…")
        case "models": return qsTr("Downloading face models…")
        case "runtime": return qsTr("Finding faces needs Python OpenCV and NumPy.")
        case "error": return qsTr("Finding faces ran into an error and will try again.")
        case "ready": return status.errors ? qsTr("Thumbnails that could not be read: %1.").arg(count(status.errors)) : ""
        default: return qsTr("Waiting for the face finder…")
        }
    }
    Timer {
        interval: 3000
        repeat: true
        running: panel.shown && !panel.picking && !pinhole.away
        onTriggered: { if (!panel.readJob && !panel.assignmentJob) panel.load() }
    }

    function go(next, row) {
        view = next
        anchor = next === "group" ? row : null
        person = next === "person" || next === "check" ? (typeof row === "string" ? row : row.name) : ""
        faces = []; likely = []
        faceLimit = 96
        checking = 0
        selected = 0
        load()
        grid.contentY = 0
    }
    function enter(row) {
        if (!row) return
        if (view !== "home") {
            if (pinhole.library.indexOf(row.id) < 0) return
            const open = rows => {
                const ids = rows.map(face => face.id).filter((id, i, all) => all.indexOf(id) === i && pinhole.library.indexOf(id) >= 0)
                pinhole.openPhotos(ids.includes(row.id) ? ids : [row.id], row.id, { comeBack: true })
            }
            if (!moreFaces) {
                open(faces)
                return
            }
            // All of them for ← →, not only those scrolled to yet.
            const operation = view === "person" ? "person" : "members"
            const owner = view === "group" ? anchor?.anchor : view === "check" ? likely[checking]?.anchor : null
            request(operation, { anchor: owner, person: person, limit: 2000 })
                .then(result => { if (shown && !pinhole.away) open(JSON.parse(result.stdout).rows) })
                .catch(() => { if (shown && !pinhole.away) open(faces) })
        } else if (row.name) {
            go("person", row)
        } else {
            go("group", row)
        }
    }
    function back() {
        if (picking) closePicker()
        else if (view === "home") close()
        else go("home")
    }
    function openPicker(mode) {
        stopReading()
        picking = mode
        failure = ""
        picker.reset()
    }
    function closePicker(restoreFocus = true) {
        assignmentJob?.cancel()
        assignmentJob = null
        picking = ""
        if (restoreFocus) grid.forceActiveFocus()
    }
    function writeEdits(key, patch) {
        const saved = Object.assign({}, pinhole.state[key] || {})
        for (const face of Object.keys(patch)) {
            const edit = patch[face]
            if (edit.name || edit.person || edit.rejected?.length || edit.not?.length || edit.unknown) saved[face] = edit
            else delete saved[face]
        }
        pinhole.state[key] = saved
        if (stateKey === key) {
            edits = saved
            if (shown) load()
        }
    }
    function edit(patch, label) {
        const key = stateKey
        const before = {}
        for (const face of Object.keys(patch))
            before[face] = Object.assign({}, edits[face])
        writeEdits(key, patch)
        pinhole.did(label, () => panel.writeEdits(key, before), () => panel.writeEdits(key, patch))
    }
    // The whole group `row` is `name`; then on to the next one to name.
    // After a group is dealt with: the next one still to name, else home.
    function nextGroup(row) {
        const at = groups.findIndex(group => group.anchor === row.anchor)
        const next = row.unknown ? null : groups[at + 1] ?? null
        if (next) {
            anchor = next
            faces = []
            selected = 0
        } else {
            view = "home"
            anchor = null
        }
    }
    // Not known for now: on to the next, this one kept apart at the end of
    // home, to name whenever its name comes back. Again: known after all.
    function markUnknown() {
        const row = anchor
        if (view !== "group" || !row) return
        const previous = edits[row.anchor] || {}
        const now = !row.unknown
        if (now) nextGroup(row)
        else anchor = Object.assign({}, row, { unknown: false })
        edit({ [row.anchor]: Object.assign({}, previous, { unknown: now || undefined }) },
             now ? qsTr("marking a face as not known") : qsTr("unmarking a face as not known"))
    }
    function nameGroup(row, name) {
        if (!row || !name || assignmentJob) return
        const job = request("assignment", { anchor: row.anchor })
        assignmentJob = job
        job.then(result => {
            if (assignmentJob !== job) return
            assignmentJob = null
            const patch = {}
            for (const face of JSON.parse(result.stdout).keys)
                patch[face] = Object.assign({}, edits[face], { person: name })
            const added = Object.keys(patch).length
            if (view === "group" && anchor?.anchor === row.anchor)
                nextGroup(row)
            if (added) {
                edit(patch, qsTr("naming a group"))
                pinhole.notify(qsTr("%1: %n photo(s) added", "", added).arg(name))
            } else {
                load()
            }
        }).catch(error => {
            if (assignmentJob !== job) return
            assignmentJob = null
            failure = qsTr("People could not load the face groups.")
            pinhole.warn(error)
        })
    }
    // While checking: this group is not `person`, ask about the next.
    function refuse() {
        const row = likely[checking]
        if (!row) return
        const previous = edits[row.anchor] || {}
        edit({ [row.anchor]: Object.assign({}, previous, { not: (previous.not || []).concat([person]) }) },
             qsTr("saying a group is someone else"))
    }
    function skip() {
        if (checking + 1 < likely.length) {
            ++checking
            faces = []
            selected = 0
            load()
        } else {
            go("person", person)
        }
    }
    function choose(name) {
        if (view === "group") nameGroup(anchor, name)
        else if (view === "check") nameGroup(likely[checking], name)
    }
    // One photo out of what is shown: not this group's person, or not the
    // named person.
    function removeSelected() {
        const face = selection
        if (!face || view === "home") return
        if (view === "person") {
            const previous = edits[face.key] || {}
            if (previous.person) {
                edit({ [face.key]: Object.assign({}, previous, { person: undefined }) }, qsTr("taking a photo from a person"))
                return
            }
            // Named with a whole group by an older version: out of that group.
            const patch = {}
            for (const key of Object.keys(edits)) {
                if (edits[key].name !== person || key === face.key) continue
                patch[key] = Object.assign({}, edits[key], { rejected: (edits[key].rejected || []).concat([face.key]) })
            }
            if (Object.keys(patch).length) edit(patch, qsTr("taking a photo from a person"))
            return
        }
        const owner = view === "group" ? anchor : likely[checking]
        if (!owner || face.key === owner.anchor) return
        const previous = edits[owner.anchor] || {}
        edit({ [owner.anchor]: Object.assign({}, previous, {
            rejected: (previous.rejected || []).concat([face.key])
        }) }, qsTr("removing a face from a group"))
    }
    function assignPicked(name) {
        if (picking === "photo") {
            const face = selection
            closePicker()
            if (face) edit({ [face.key]: Object.assign({}, edits[face.key], { person: name }) }, qsTr("assigning a photo"))
        } else if (picking === "merge") {
            const from = person
            const patch = {}
            for (const face of Object.keys(edits)) {
                const previous = edits[face]
                if (previous.name === from || previous.person === from) {
                    patch[face] = Object.assign({}, previous)
                    if (previous.name === from) patch[face].name = name
                    if (previous.person === from) patch[face].person = name
                }
            }
            closePicker()
            person = name
            edit(patch, qsTr("merging people"))
        } else if (picking === "name") {
            closePicker()
            choose(name)
        }
    }
    // Arrows across both of home's grids: down from a named person to the
    // groups below in the same column.
    // Arrows across home's grids: down from a named person to the faces
    // below in the same column, and on to those not known.
    function move(dx, dy) {
        if (!tiles.length) return
        let next = selected + dx
        if (dy !== 0) {
            const bounds = view === "home"
                ? [0, people.length, people.length + unknown.length, tiles.length].filter((b, i, all) => i === 0 || b > all[i - 1])
                : [0, tiles.length]
            let section = 0
            while (section + 2 < bounds.length && selected >= bounds[section + 1]) ++section
            const start = bounds[section], end = bounds[section + 1]
            const column = (selected - start) % columns
            next = selected + dy * columns
            if (next < start && section > 0) {
                const from = bounds[section - 1], last = start - 1
                next = Math.min(last, from + Math.floor((last - from) / columns) * columns + column)
            } else if (next >= end && section + 2 < bounds.length) {
                next = Math.min(bounds[section + 2] - 1, end + column)
            }
        }
        selected = Math.max(0, Math.min(tiles.length - 1, next))
        reveal(selected)
        if (view === "home" && selected >= tiles.length - columns * 2 && moreGroups) more()
        if (view !== "home" && selected >= faces.length - columns * 2 && moreFaces) more()
    }
    function reveal(index) {
        const item = view !== "home" ? faceTiles.itemAt(index)
            : index < people.length ? namedTiles.itemAt(index)
            : index < people.length + unknown.length ? unknownTiles.itemAt(index - people.length)
            : groupTiles.itemAt(index - people.length - unknown.length)
        if (!item) return
        const top = item.mapToItem(content, 0, 0).y
        const bottom = top + item.height + theme.fontSize * 2.4
        if (top < grid.contentY) grid.contentY = Math.max(0, top - 12)
        else if (bottom > grid.contentY + grid.height) grid.contentY = bottom - grid.height
    }
    function more() {
        if (readJob) return
        if (view === "home") groupLimit += 48
        else faceLimit += 96
        load()
    }

    function tabKey(event) {
        return (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
    }
    Keys.onShortcutOverride: event => { if (tabKey(event)) event.accepted = true }
    Keys.onPressed: event => {
        if (!tabKey(event)) return
        const candidates = picking ? [picker.searchField, picker.choicesList, picker.createButton, picker.cancelButton]
            : [backButton, ...nameButtons.children, newName.input, mergeButton, checkButton]
        const controls = candidates.filter(item => item && item.visible && item.activeFocusOnTab && item.usable !== false)
        const reverse = event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier)
        const current = controls.indexOf(panel.Window.window.activeFocusItem)
        const index = current < 0 ? (reverse ? controls.length - 1 : 0)
            : (current + (reverse ? -1 : 1) + controls.length) % controls.length
        if (current >= 0 && ((reverse && index === controls.length - 1) || (!reverse && index === 0)))
            grid.forceActiveFocus()
        else
            controls[index]?.forceActiveFocus(reverse ? Qt.BacktabFocusReason : Qt.TabFocusReason)
        event.accepted = true
    }
    readonly property bool keysOnTiles: shown && !picking && grid.activeFocus
    Command { name: "people.back"; keys: ["Escape"]; keyLayer: "plugin"; when: panel.shown; onTriggered: panel.back() }
    Command { name: "people.enter"; keys: ["Return", "Enter", "Space"]; keyLayer: "plugin"; when: panel.keysOnTiles; onTriggered: panel.enter(panel.selection) }
    Command { name: "people.left"; keys: ["Left"]; keyLayer: "plugin"; when: panel.keysOnTiles; autoRepeat: true; onTriggered: panel.move(-1, 0) }
    Command { name: "people.right"; keys: ["Right"]; keyLayer: "plugin"; when: panel.keysOnTiles; autoRepeat: true; onTriggered: panel.move(1, 0) }
    Command { name: "people.up"; keys: ["Up"]; keyLayer: "plugin"; when: panel.keysOnTiles; autoRepeat: true; onTriggered: panel.move(0, -1) }
    Command { name: "people.down"; keys: ["Down"]; keyLayer: "plugin"; when: panel.keysOnTiles; autoRepeat: true; onTriggered: panel.move(0, 1) }
    Command {
        id: removeCommand
        name: "people.remove"
        text: panel.view === "person" ? qsTr("Not %1").arg(panel.person) : qsTr("Someone else")
        keys: ["X", "Delete"]
        keyLayer: "plugin"
        when: panel.keysOnTiles && panel.view !== "home" && panel.selection !== null
            && (panel.view === "person" || panel.selection.key !== (panel.view === "group" ? panel.anchor?.anchor : panel.likely[panel.checking]?.anchor))
        onTriggered: panel.removeSelected()
    }
    Command {
        id: moveCommand
        name: "people.move"
        text: qsTr("Someone named…")
        keys: ["M"]
        keyLayer: "plugin"
        when: panel.keysOnTiles && panel.view === "person" && panel.selection !== null && panel.names.length > 1
        onTriggered: panel.openPicker("photo")
    }
    Command {
        id: nameCommand
        name: "people.name"
        text: qsTr("New name")
        keys: ["N"]
        keyLayer: "plugin"
        when: panel.keysOnTiles && panel.view === "group"
        onTriggered: newName.input.forceActiveFocus()
    }
    Command {
        id: checkCommand
        name: "people.check"
        text: qsTr("Check them")
        keys: ["C"]
        keyLayer: "plugin"
        when: panel.shown && !panel.picking && panel.view === "person" && (panel.personRow?.likely ?? 0) > 0
        onTriggered: panel.go("check", panel.person)
    }
    Command {
        id: unknownCommand
        name: "people.unknown"
        text: qsTr("Don't know")
        keys: ["0"]
        keyLayer: "plugin"
        when: panel.shown && !panel.picking && panel.view === "group" && !newName.input.activeFocus
        onTriggered: panel.markUnknown()
    }
    Command {
        id: refuseCommand
        name: "people.refuse"
        text: qsTr("No")
        keys: ["0"]
        keyLayer: "plugin"
        when: panel.shown && !panel.picking && panel.view === "check" && panel.likely.length > 0 && !newName.input.activeFocus
        onTriggered: panel.refuse()
    }
    Command {
        id: skipCommand
        name: "people.skip"
        text: qsTr("Skip")
        keys: ["S"]
        keyLayer: "plugin"
        when: panel.shown && !panel.picking && panel.view === "check" && panel.likely.length > 0 && !newName.input.activeFocus
        onTriggered: panel.skip()
    }
    Repeater {
        model: 9
        delegate: Item {
            required property int index
            Command {
                name: "people.choose." + (index + 1)
                keys: [String(index + 1)]
                keyLayer: "plugin"
                when: panel.shown && !panel.picking && (panel.view === "group" || panel.view === "check")
                    && index < panel.choices.length && !newName.input.activeFocus
                onTriggered: panel.choose(panel.choices[index])
            }
        }
    }
    Connections {
        target: pinhole
        function onDismissed() { panel.close() }
        function onHiddenChanged() { if (panel.shown) panel.load() }
        // Back from the photos, on the one last looked at.
        function onReturned(photo) {
            const at = panel.faces.findIndex(face => face.id === photo)
            if (at >= 0) panel.selected = at
            grid.forceActiveFocus()
            Qt.callLater(() => panel.reveal(panel.selected))
        }
    }
    Connections {
        target: pinhole.library
        function onChanged() {
            if (panel.shown && (pinhole.library.key || pinhole.library.dir) !== panel.source)
                panel.close()
        }
    }
    PanelHeader {
        id: titlebar
        visible: !panel.picking
        title: panel.view === "home" ? qsTr("People")
            : panel.view === "group" ? qsTr("Who is this?")
            : panel.view === "check" ? qsTr("Is this %1?").arg(panel.person)
            : panel.person
        detail: panel.view === "group" && panel.anchor ? qsTr("%n photo(s)", "", panel.anchor.count)
            : panel.view === "person" && panel.personRow ? qsTr("%n photo(s)", "", panel.personRow.count)
            : panel.view === "check" && panel.likely.length ? qsTr("%1 of %2").arg(panel.checking + 1).arg(panel.likely.length)
            : ""
        // Esc goes back a level; ✕ closes from any.
        closeHint: panel.view === "home" ? "Esc" : ""
        onClosed: panel.close()
        // Faces found so far, while they are being found.
        progress: panel.status.phase === "scanning" && panel.status.total > 0
            ? Math.min(1, (panel.status.done || 0) / Math.max(1, panel.status.total)) : -1
        tools: [
            ShellButton {
                id: checkButton
                objectName: "peopleCheck"
                visible: panel.view === "person" && (panel.personRow?.likely ?? 0) > 0
                text: qsTr("%n more to check", "", panel.personRow?.likely ?? 0)
                hint: checkCommand.keyName
                onTapped: panel.go("check", panel.person)
            },
            ShellButton {
                id: mergeButton
                objectName: "peopleMerge"
                visible: panel.view === "person"
                usable: panel.names.length > 1
                text: qsTr("Merge with…")
                hint: ""
                onTapped: panel.openPicker("merge")
            }
        ]
        ShellButton {
            id: backButton
            objectName: "peopleBack"
            visible: panel.view !== "home"
            text: "‹ " + qsTr("People")
            hint: "Esc"
            onTapped: panel.back()
        }
        // Whom the faces below are compared with.
        PeopleTile {
            anchors.verticalCenter: parent.verticalCenter
            visible: (panel.view === "check" || panel.view === "person") && panel.personRow !== null
            width: visible ? titlebar.controlHeight : 0
            height: width
            row: panel.personRow ?? { id: "", box: [0, 0, 1, 1] }
            enabled: false
        }
    }

    // A group's or a checked group's names: the likeliest first, each with
    // its number's key, then a new one, all on one row of controls of one
    // height, wrapping as a sentence does.
    Item {
        id: namebar
        visible: !panel.picking && (panel.view === "group" || panel.view === "check")
        anchors.top: titlebar.bottom
        width: parent.width
        // As far below the title's bar as the names are from the photos.
        height: visible ? nameButtons.height + 24 : 0
        Flow {
            id: nameButtons
            x: 12
            y: 12
            width: parent.width - 24
            spacing: 6
            Repeater {
                model: panel.choices
                delegate: ShellButton {
                    required property string modelData
                    required property int index
                    objectName: "peopleChoice"
                    text: panel.view === "check" && index === 0 ? qsTr("Yes") : modelData
                    hint: String(index + 1)
                    active: index === 0 && (panel.view === "check" || !!panel.anchor?.guess)
                    busy: !!panel.assignmentJob
                    onTapped: panel.choose(modelData)
                }
            }
            ShellButton {
                visible: panel.view === "group"
                objectName: "peopleUnknown"
                text: unknownCommand.text
                hint: unknownCommand.keyName
                active: !!panel.anchor?.unknown
                checkable: true
                onTapped: panel.markUnknown()
            }
            ShellButton {
                visible: panel.view === "check"
                objectName: "peopleRefuse"
                text: refuseCommand.text
                hint: refuseCommand.keyName
                onTapped: panel.refuse()
            }
            ShellButton {
                visible: panel.view === "check"
                text: skipCommand.text
                hint: skipCommand.keyName
                onTapped: panel.skip()
            }
            FieldInput {
                id: newName
                objectName: "peopleNewName"
                visible: panel.view === "group"
                width: Math.min(theme.fontSize * 16, nameButtons.width)
                height: titlebar.controlHeight
                placeholder: qsTr("New name…")
                input.maximumLength: 100
                onAccepted: {
                    const typed = text.trim()
                    if (!typed) return
                    const existing = panel.names.find(name => name.toLowerCase() === typed.toLowerCase())
                    text = ""
                    grid.forceActiveFocus()
                    panel.choose(existing || typed)
                }
                onCleared: grid.forceActiveFocus()
            }
        }
    }

    Item {
        id: statusLine
        // How far the faces are found, at home; a group's page shows only
        // the bar under its title.
        visible: !panel.picking && statusText.text !== "" && (panel.view === "home" || panel.failure !== "")
        anchors.top: namebar.bottom
        width: parent.width
        height: visible ? statusText.implicitHeight + 10 : 0
        UiText {
            id: statusText
            x: 14
            width: parent.width - 28
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.WordWrap
            color: panel.failure ? theme.red : theme.darkForeground
            text: panel.failure || panel.statusText()
        }
    }

    ScrollArea {
        id: grid
        objectName: "peopleGrid"
        visible: !panel.picking
        anchors.top: statusLine.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        contentWidth: width
        contentHeight: content.height + 24
        onContentYChanged: {
            if (contentY + height > contentHeight - height / 2 && (panel.view === "home" ? panel.moreGroups : panel.moreFaces))
                panel.more()
        }
        Column {
            id: content
            x: 12
            width: grid.width - 24
            spacing: 14
            readonly property real cell: width / panel.columns

            Grid {
                visible: panel.view === "home" && panel.people.length > 0
                columns: panel.columns
                Repeater {
                    id: namedTiles
                    model: panel.view === "home" ? panel.people : []
                    delegate: PersonCircle {
                        required property var modelData
                        required property int index
                        width: content.cell
                        row: modelData
                        caption: modelData.name
                        detail: qsTr("%n photo(s)", "", modelData.count)
                        chosen: panel.selected === index && grid.activeFocus && keymap.input === "keyboard"
                        onSelected: {
                            panel.selected = index
                            grid.forceActiveFocus()
                            panel.enter(modelData)
                        }
                    }
                }
            }
            UiText {
                visible: panel.view === "home" && panel.unknown.length > 0
                topPadding: 8
                color: theme.brightForeground
                text: qsTr("Not known yet")
                width: parent.width
            }
            Grid {
                visible: panel.view === "home"
                columns: panel.columns
                Repeater {
                    id: unknownTiles
                    model: panel.view === "home" ? panel.unknown : []
                    delegate: PersonCircle {
                        required property var modelData
                        required property int index
                        readonly property int at: panel.people.length + index
                        width: content.cell
                        row: modelData
                        opacity: chosen || hovered ? 1 : 0.6
                        caption: qsTr("%n photo(s)", "", modelData.count)
                        chosen: panel.selected === at && grid.activeFocus && keymap.input === "keyboard"
                        onSelected: {
                            panel.selected = at
                            grid.forceActiveFocus()
                            panel.enter(modelData)
                        }
                    }
                }
            }
            UiText {
                visible: panel.view === "home" && panel.known.length > 0
                topPadding: panel.people.length ? 8 : 0
                color: theme.brightForeground
                text: panel.people.length ? qsTr("Not named yet") : qsTr("Name the faces Pinhole found to see each person's photos together.")
                width: parent.width
                wrapMode: Text.WordWrap
            }
            Grid {
                visible: panel.view === "home"
                columns: panel.columns
                Repeater {
                    id: groupTiles
                    model: panel.view === "home" ? panel.known : []
                    delegate: PersonCircle {
                        required property var modelData
                        required property int index
                        width: content.cell
                        row: modelData
                        caption: modelData.guess ? qsTr("%1?").arg(modelData.guess) : qsTr("%n photo(s)", "", modelData.count)
                        detail: modelData.guess ? qsTr("%n photo(s)", "", modelData.count) : ""
                        guessed: !!modelData.guess
                        chosen: panel.selected === panel.people.length + panel.unknown.length + index && grid.activeFocus && keymap.input === "keyboard"
                        onSelected: {
                            panel.selected = panel.people.length + panel.unknown.length + index
                            grid.forceActiveFocus()
                            panel.enter(modelData)
                        }
                    }
                }
            }
            Grid {
                visible: panel.view !== "home"
                columns: panel.columns
                Repeater {
                    id: faceTiles
                    model: panel.view !== "home" ? panel.faces : []
                    delegate: Item {
                        required property var modelData
                        required property int index
                        width: content.cell
                        height: content.cell
                        PeopleTile {
                            id: photoTile
                            anchors.fill: parent
                            anchors.margins: 2.5
                            row: modelData
                            round: false
                            chosen: panel.selected === index && grid.activeFocus && keymap.input === "keyboard"
                            Accessible.name: pinhole.library.item(modelData.id).filename || qsTr("Photo %1").arg(index + 1)
                            onSelected: {
                                panel.selected = index
                                grid.forceActiveFocus()
                                panel.enter(modelData)
                            }
                        }
                        // Under the pointer, what the keys do to the chosen one.
                        Row {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 8
                            spacing: 4
                            visible: (photoTile.hovered || tileButtons.hovered) && keymap.input !== "keyboard"
                            HoverHandler { id: tileButtons }
                            ShellButton {
                                visible: panel.view === "person" && panel.names.length > 1
                                floating: true
                                text: "…"
                                helpText: moveCommand.text
                                hint: ""
                                onTapped: {
                                    panel.selected = index
                                    panel.openPicker("photo")
                                }
                            }
                            ShellButton {
                                visible: panel.view === "person" || modelData.key !== (panel.view === "group" ? panel.anchor?.anchor : panel.likely[panel.checking]?.anchor)
                                floating: true
                                text: "✕"
                                helpText: removeCommand.text
                                hint: ""
                                onTapped: {
                                    panel.selected = index
                                    panel.removeSelected()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    UiText {
        anchors.centerIn: grid
        width: Math.min(grid.width - 48, 460)
        visible: !panel.picking && !panel.readJob && panel.tiles.length === 0 && panel.status.phase === "ready"
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        font.pixelSize: Math.round(theme.fontSize * 1.35)
        color: theme.brightForeground
        text: panel.view === "home" ? qsTr("No faces found in this library yet.") : qsTr("No photos here any more.")
    }

    PeoplePicker {
        id: picker
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, theme.fontSize * 34)
        height: Math.min(parent.height - 24, theme.fontSize * 36)
        visible: !!panel.picking
        names: panel.names.filter(name => name !== (panel.view === "person" ? panel.person : ""))
        busy: !!panel.assignmentJob
        allowCreate: panel.picking !== "merge"
        title: panel.failure || (panel.picking === "merge" ? qsTr("Merge %1 with").arg(panel.person)
            : qsTr("Who is in this photo?"))
        onChosen: name => panel.assignPicked(name)
        onCancelled: panel.closePicker()
    }
}
