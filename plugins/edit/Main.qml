import QtQuick
import Pinhole.Plugin

// Turn and crop: R turns the photo shown a quarter clockwise, Shift R a
// quarter back, and K crops it (Crop.qml). What is ticked turns too, from
// select mode's bar. Kept in Pinhole, never in the photo's file
// (pinhole.adjust): the grid, the viewer, an export as JPEG and a copy show
// it so; the original exported is as it was taken. Each is a step Ctrl Z
// undoes.
PinholePlugin {
    id: plugin
    readonly property bool viewing: pinhole.viewer.open && pinhole.viewer.index >= 0
    readonly property bool editable: viewing && !pinhole.viewer.photo.video
    // The photo shown turned or cropped here: Original takes it back.
    readonly property bool adjusted: (pinhole.library.revision, editable && (() => {
        const now = pinhole.adjustment(pinhole.viewer.index)
        return now.turn !== 0 || now.crop.x !== 0 || now.crop.y !== 0 || now.crop.width !== 1 || now.crop.height !== 1
    })())
    // As it was taken: the turn and the crop taken back, a step to undo.
    function original(photos) {
        const ids = idsOf(photos)
        if (ids.length > 0)
            step(qsTr("going back to the original"), ids, () => pinhole.adjust(ids, { turn: 0, crop: null }))
    }

    function idsOf(photos) {
        return photos.map(photo => pinhole.library.item(photo).id).filter(id => id !== undefined)
    }
    // Saving in iCloud, which takes a second or two: said if it takes
    // longer than a moment.
    property bool saving: false
    property bool said: false
    Timer {
        id: slow
        interval: 700
        onTriggered: if (plugin.saving) { plugin.said = true; pinhole.notify(qsTr("Saving the edit in iCloud…")) }
    }
    // Each as it is now, to undo to; then done (in iCloud, where the photo
    // is iCloud's, else kept here), and a step that puts them back, or does
    // it again, in the library it was done in.
    function step(words, ids, apply) {
        const before = ids.map(id => pinhole.adjustment(id))
        const key = pinhole.library.key
        saving = true
        said = false
        slow.restart()
        apply().then(done => {
            saving = false
            const after = done.photos
            pinhole.did(words,
                        () => { if (pinhole.library.key === key) ids.forEach((id, i) => pinhole.adjust(id, before[i]).catch(pinhole.warn)) },
                        () => { if (pinhole.library.key === key) after.forEach(a => pinhole.adjust(a.id, a).catch(pinhole.warn)) })
            if (said && done.kept === "source")
                pinhole.notify(qsTr("Saved in iCloud"))
        }).catch(error => {
            saving = false
            pinhole.notify(qsTr("Could not save the edit: %1").arg(error.message))
        })
    }
    function turn(photos, quarters) {
        const ids = idsOf(photos)
        if (ids.length > 0)
            step(qsTr("turning"), ids, () => pinhole.turn(ids, quarters))
    }
    function crop(photo, adjustment) {
        const ids = idsOf([photo])
        if (ids.length > 0)
            step(qsTr("cropping"), ids, () => pinhole.adjust(ids[0], adjustment))
    }

    actions: [
        PluginAction {
            place: "viewer"
            order: 10
            text: qsTr("Turn")
            key: "R"
            shown: plugin.editable
            onTriggered: plugin.turn([pinhole.viewer.index], 1)
        },
        PluginAction {
            place: "viewer"
            order: 11
            text: qsTr("Crop")
            key: "K"
            shown: plugin.editable
            onTriggered: cropping.open(pinhole.viewer.id)
        },
        PluginAction {
            place: "viewer"
            order: 12
            text: qsTr("Original")
            key: "Shift+K"
            shown: plugin.adjusted
            onTriggered: plugin.original([pinhole.viewer.index])
        },
        PluginAction {
            place: "selection"
            text: qsTr("Turn")
            usable: pinhole.selectionCount > 0
            onTriggered: plugin.turn(Array.from(pinhole.selection()), 1)
        },
        PluginAction {
            place: "selection"
            text: qsTr("Original")
            usable: pinhole.selectionCount > 0
            onTriggered: plugin.original(Array.from(pinhole.selection()))
        }
    ]
    help: [["R", qsTr("Turn the photo a quarter; ⇧ R back")], ["K", qsTr("Crop the photo")],
           ["⇧ K", qsTr("The photo as it was taken again")]]
    // A quarter back: a key, beside Turn's button.
    Command {
        name: "edit.turnBack"
        text: qsTr("Turn back")
        section: "plugin"
        keys: ["Shift+R"]
        keyLayer: "viewer"
        noMouse: qsTr("Turn three times")
        noTouchpad: qsTr("Turn three times")
        when: plugin.editable
        onTriggered: plugin.turn([pinhole.viewer.index], -1)
    }

    PluginView { id: cropping; objectName: "cropping"; source: "Crop.qml"; toggleKey: "K"; closeText: qsTr("Leave it as it was") }
}
