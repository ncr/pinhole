import QtQml
import QtQuick

// One place of a PluginAction with `places` (API 15): drawn in that place as
// any action is, with its own button, standing for `of`, whose words, key and
// state it shows. Pressed, it says where and on which photos (`of.pressedIn`,
// `of.photos`), then triggers `of`. Made by PluginPlane, never by a plugin.
PluginAction {
    id: placed
    required property var of
    // The plugin's host (its pinhole): what is selected, what shows.
    required property var host
    required property string where

    place: where
    group: of.group
    text: of.text
    key: of.key
    hint: of.hint
    order: of.order
    textColour: of.textColour
    active: of.active
    busy: of.busy
    busyDelay: of.busyDelay
    shown: of.shown && (where !== "viewer" || host.viewer.open)
    // Where there is something to do it on: a photo shown, photos ticked,
    // photos at all. One on its way stays pressable, to show how far.
    readonly property bool hasPhotos: where === "selection" ? host.selectionCount > 0
                                    : where === "viewer" ? host.viewer.id !== ""
                                    : host.library.count > 0
    usable: of.usable && (of.busy || hasPhotos)
    onHoveredChanged: of.hovered = hovered
    // A tour lights the button where it shows.
    onButtonChanged: if (button) of.button = button
    onTriggered: {
        of.pressedIn = where
        of.photos = where === "selection" ? host.selectionIds()
                  : where === "viewer" ? (host.viewer.id !== "" ? [host.viewer.id] : [])
                  : []
        of.triggered()
    }
    // What it could not do, under the button it was pressed with.
    property Connections complaints: Connections {
        target: placed.of
        function onComplained(text) {
            if (placed.of.pressedIn === placed.where)
                placed.complain(text)
        }
    }
}
