import QtQuick
import Pinhole.Plugin

// People: the faces Pinhole found, named in a view of their own (Shift+F,
// PeoplePanel.qml) and on the photo shown full window: a button there says
// who is in it, and W (or the button) names them (WhoView.qml).
PinholePlugin {
    id: plugin

    // What is said of faces here and in the view, as pinhole-faces takes it:
    // by face, synced with the library to every computer it is shown on.
    readonly property string source: pinhole.library.key || pinhole.library.dir
    property var edits: ({})
    // What is said of faces changed, here or elsewhere: the view reads
    // again.
    signal edited()

    function reread() {
        edits = pinhole.synced.values()
        edited()
        if (shownId !== "") lookSoon.restart()
    }
    Connections {
        target: pinhole.synced
        function onChanged() { plugin.reread() }
    }
    // Kept on this computer alone before the names went with the library
    // (pinhole.state, "people:" and the library's key): taken into it once,
    // what is synced already of a face winning.
    function adopt() {
        const key = "people:" + source
        const before = pinhole.state[key]
        if (!before) return
        const now = pinhole.synced.values()
        for (const face of Object.keys(before))
            if (now[face] === undefined) pinhole.synced[face] = before[face]
        pinhole.state[key] = null
    }
    Component.onCompleted: { adopt(); reread() }
    onSourceChanged: { adopt(); reread() }

    function writeEdits(patch) {
        const now = Object.assign({}, edits)
        for (const face of Object.keys(patch)) {
            const edit = patch[face]
            const kept = edit.name || edit.person || edit.rejected?.length || edit.not?.length || edit.unknown
            pinhole.synced[face] = kept ? edit : null
            if (kept) now[face] = edit
            else delete now[face]
        }
        // There at once for the next step; told again as it is kept.
        edits = now
    }
    // Said of faces since found again in a new thumbnail (turned or cropped
    // in iCloud, edited on a phone): moved to their keys now, as
    // pinhole-faces says ({ old: new }). No step: nothing changes but the
    // keys. Another computer finds the same new keys in the same
    // thumbnails, and moves what it hears of the old ones as it reads them.
    function moveEdits(moved) {
        const olds = Object.keys(moved || {})
        if (olds.length === 0) return
        const patch = {}
        for (const old of olds) {
            if (!(old in edits)) continue
            if (!(moved[old] in edits)) patch[moved[old]] = edits[old]
            patch[old] = null
        }
        for (const key of Object.keys(edits)) {
            const rejected = edits[key].rejected
            if (rejected && rejected.some(face => face in moved))
                patch[key] = Object.assign({}, patch[key] || edits[key], { rejected: rejected.map(face => moved[face] || face) })
        }
        const now = Object.assign({}, edits)
        for (const key of Object.keys(patch)) {
            pinhole.synced[key] = patch[key]
            if (patch[key] === null) delete now[key]
            else now[key] = patch[key]
        }
        edits = now
    }
    // A step to undo.
    function edit(patch, label) {
        const before = {}
        for (const face of Object.keys(patch))
            before[face] = Object.assign({}, edits[face])
        writeEdits(patch)
        pinhole.did(label, () => plugin.writeEdits(before), () => plugin.writeEdits(patch))
    }

    // The photo shown full window, and the faces in it: [{ key, box, name,
    // guess, anchor, legacy, drawn }], `drawn` the face's box in the photo as
    // it is turned and cropped here, left to right.
    readonly property string shownId: (pinhole.library.revision, pinhole.viewer.open && pinhole.viewer.index >= 0
                                       ? pinhole.viewer.id : "")
    property string facesOf: ""
    // As pinhole-faces gave them; placed again as the photo is turned or
    // cropped (the library read again).
    property var found: []
    readonly property var faces: (pinhole.library.revision, facesOf !== "" ? placed(facesOf, found) : [])
    // Every name given, the most photos first.
    property var names: []
    readonly property bool hasFaces: facesOf !== "" && facesOf === shownId && faces.length > 0

    // Asked a moment after a photo shows, not for each one passed.
    onShownIdChanged: {
        if (shownId !== facesOf) {
            facesOf = ""
            found = []
        }
        looking.cancel()
        if (shownId !== "") lookSoon.restart()
    }
    Timer { id: lookSoon; interval: 200; onTriggered: plugin.look() }
    function look() {
        const id = shownId
        if (id === "") return
        looking.start(() => pinhole.run("pinhole-faces", ["photo", "--cache", pinhole.cacheDir], {
            input: JSON.stringify({ source: plugin.source, edits: edits, photo: id, offset: 0 }),
            timeout: 10000
        }), result => {
            if (id !== plugin.shownId) return
            const response = JSON.parse(result.stdout)
            plugin.moveEdits(response.moved)
            plugin.names = response.names || []
            plugin.found = response.rows || []
            plugin.facesOf = id
        })
    }
    Job {
        id: looking
        onFailed: error => pinhole.warn(error)
    }
    // The faces found since: once the finder is done again.
    Connections {
        target: pinhole
        enabled: plugin.shownId !== ""
        function onWorkerSaid(name, said) {
            if (name === "faces" && said.event === "status" && said.phase === "ready") plugin.look()
        }
    }

    // Each face where it is in the photo as drawn here (turned by quarters
    // clockwise, then cut: pinhole.adjustment), the ones cut away left out.
    // An edit the source keeps (iCloud's) is in its thumbnail already, the
    // faces found where it has them: only one kept here is drawn on.
    function placed(id, rows) {
        const kept = pinhole.adjustment(id)
        const adjusted = kept.kept === "here" ? kept : { turn: 0, crop: null }
        const crop = adjusted.crop || { x: 0, y: 0, width: 1, height: 1 }
        return rows.map(row => {
            let [x, y, w, h] = row.box
            for (let i = 0; i < (adjusted.turn || 0); ++i)
                [x, y, w, h] = [1 - y - h, x, h, w]
            const drawn = [(x - crop.x) / crop.width, (y - crop.y) / crop.height, w / crop.width, h / crop.height]
            return Object.assign({}, row, { drawn: drawn })
        }).filter(face => {
            const cx = face.drawn[0] + face.drawn[2] / 2, cy = face.drawn[1] + face.drawn[3] / 2
            return cx > 0 && cx < 1 && cy > 0 && cy < 1
        }).sort((a, b) => a.drawn[0] - b.drawn[0] || a.drawn[1] - b.drawn[1])
    }

    // On the button: who is in it, a likely one with a ?, the rest counted.
    readonly property string whoText: {
        const parts = []
        let others = 0
        for (const face of faces) {
            const part = face.name ? face.name : face.guess ? qsTr("%1?").arg(face.guess) : ""
            if (part === "") ++others
            else if (!parts.includes(part)) parts.push(part)
        }
        if (parts.length === 0) return faces.length > 1 ? qsTr("Who are they?") : qsTr("Who is this?")
        const shown = parts.slice(0, 2)
        const more = parts.length - shown.length + others
        return more > 0 ? qsTr("%1 +%2").arg(shown.join(", ")).arg(more) : shown.join(", ")
    }

    // What a face is now, shown at once (the next one to name chosen by
    // it), until pinhole-faces says so too.
    function said(key, name) {
        found = found.map(row => row.key === key ? Object.assign({}, row, { name: name, guess: null }) : row)
    }
    // This face is `name`, alone: its group stays to name in People.
    function nameFace(face, name) {
        said(face.key, name)
        edit({ [face.key]: Object.assign({}, edits[face.key], { person: name }) }, qsTr("naming a face"))
    }
    // This face is not who it was said to be (nor guessed to be them
    // again): out of a group named by an older version too. The word kept
    // on it sets it apart from the group it waited in at the next
    // regrouping (pinhole-faces' load_edits), as one said to be someone
    // else is: the rest of that group may well be them.
    function unnameFace(face) {
        const previous = edits[face.key] || {}
        const patch = { [face.key]: Object.assign({}, previous, {
            person: undefined,
            not: (previous.not || []).filter(name => name !== face.name).concat([face.name])
        }) }
        said(face.key, null)
        if (face.legacy && face.legacy !== face.key) {
            const group = edits[face.legacy] || {}
            patch[face.legacy] = Object.assign({}, group, { rejected: (group.rejected || []).concat([face.key]) })
        }
        edit(patch, qsTr("taking a name from a face"))
    }
    // A name can be taken from it here: given to it alone, or with a group
    // named by an older version, as one of its photos.
    function canUnname(face) {
        return !!face.name && (!!edits[face.key]?.person || (!!face.legacy && face.legacy !== face.key))
    }

    actions: [
        PluginAction {
            place: "grid"
            text: qsTr("People")
            key: "Shift+F"
            shown: pinhole.library.count > 0
            onTriggered: people.open()
        },
        PluginAction {
            id: whoAction
            place: "viewer"
            // Never folded into More: who it is, at a glance.
            order: -5
            text: plugin.whoText
            key: "W"
            shown: plugin.hasFaces
            active: who.shown
            onTriggered: who.toggle()
        }
    ]
    help: [["Shift+F", qsTr("People")], ["W", qsTr("Who is in the photo shown")],
           ["1–9", qsTr("Name the group shown")], ["X", qsTr("Take the photo out")],
           ["0", qsTr("Skip a face you do not know, for later")],
           ["C", qsTr("Check the groups that look like the person shown")]]
    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 6
            SettingNote {
                text: qsTr("How readily similar faces are put in one group. Fewer groups to name, or fewer mistakes in them.")
            }
            Choices {
                numbered: false
                setting: "grouping"
                options: [{ value: "off", text: qsTr("Apart") }, { value: "careful", text: qsTr("Careful") }, { value: "wider", text: qsTr("Wider") }]
            }
        }
    }

    // Inside the photo shown: where it is on the window, for the faces to
    // be marked over it while they are named (WhoView.qml).
    property Item photoFrame: null
    viewerLayer: Item {
        id: layer
        property int photo
        property bool shown
        anchors.fill: parent
        onShownChanged: if (shown) plugin.photoFrame = layer
        Component.onCompleted: if (shown) plugin.photoFrame = layer
        Component.onDestruction: if (plugin.photoFrame === layer) plugin.photoFrame = null
    }

    PluginView { id: people; objectName: "peopleView"; source: "PeoplePanel.qml"; toggleKey: "Shift+F" }
    PluginView {
        id: who
        objectName: "peopleWho"
        source: "WhoView.qml"
        toggleKey: "W"
        closeText: qsTr("Close")
    }
}
