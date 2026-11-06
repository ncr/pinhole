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
    readonly property bool editable: viewing && !pinhole.library.item(pinhole.viewer.index).video

    function idsOf(photos) {
        return photos.map(photo => pinhole.library.item(photo).id).filter(id => id !== undefined)
    }
    // Each as it is now, to undo to; a step that puts them back, or
    // does it again, in the library it was done in.
    function step(words, ids, apply) {
        const before = ids.map(id => pinhole.adjustment(id))
        const key = pinhole.library.key
        apply()
        const after = ids.map(id => pinhole.adjustment(id))
        pinhole.did(words, () => { if (pinhole.library.key === key) ids.forEach((id, i) => pinhole.adjust(id, before[i])) },
                           () => { if (pinhole.library.key === key) ids.forEach((id, i) => pinhole.adjust(id, after[i])) })
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
            onTriggered: cropping.open(pinhole.library.item(pinhole.viewer.index).id)
        },
        PluginAction {
            place: "selection"
            text: qsTr("Turn")
            usable: pinhole.selectionCount > 0
            onTriggered: plugin.turn(Array.from(pinhole.selection()), 1)
        }
    ]
    help: [["R", qsTr("Turn the photo a quarter; ⇧ R back")], ["K", qsTr("Crop the photo")]]
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
