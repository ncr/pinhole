import QtQuick
import Pinhole.Plugin

// PINHOLE_SELFTEST=layers (app/src/selftestlayers.cpp): a layer over the
// photo that counts the times it is made and is told it is no longer shown,
// a viewer action that complains, and a select mode action before Copy.
PinholePlugin {
    id: plugin
    property int made: 0
    property int unshown: 0
    property var last: null
    property int triggered: 0
    // The action in three places: where it was last pressed, and
    // on which photos.
    property string pressedIn: ""
    property string photos: "-"

    viewerLayer: Rectangle {
        objectName: "testLayer"
        property int photo: -1
        property bool shown: false
        anchors.fill: parent
        color: "transparent"
        border.color: "#ff00ff"
        border.width: 6
        Component.onCompleted: {
            plugin.made += 1
            plugin.last = this
        }
        onShownChanged: if (!shown) plugin.unshown += 1
    }

    actions: [
        PluginAction {
            places: ["viewer", "selection", "grid"]
            text: "Everywhere"
            key: "F8"
            onTriggered: {
                plugin.pressedIn = pressedIn
                plugin.photos = photos.join(",")
            }
        },
        PluginAction {
            place: "viewer"
            text: "Test"
            key: "F9"
            onTriggered: {
                plugin.triggered += 1
                complain("Could not test")
            }
        },
        PluginAction {
            place: "selection"
            text: "Before"
            key: "F9"
            order: -5
            onTriggered: {
                plugin.triggered += 1
                complain("Nothing before")
            }
        }
    ]
}
