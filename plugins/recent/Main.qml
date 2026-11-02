import QtQuick
import Pinhole.Plugin

PinholePlugin {
    id: plugin
    function openRecent() { recent.open() }
    covering: recent.shown
    actions: [
        PluginAction {
            place: "grid"
            text: qsTr("Recent additions")
            key: "Shift+N"
            shown: pinhole.library.count > 0
            onTriggered: recent.open()
        }
    ]
    help: [["⇧ N", qsTr("Photos added recently, whatever their capture date")]]
    RecentPhotos { id: recent; anchors.fill: parent; plugin: plugin }
}
