import QtQuick
import Pinhole.Plugin

PinholePlugin {
    id: plugin
    function openRecent() { recent.open() }
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
    PluginView {
        id: recent
        source: "RecentPhotos.qml"
        toggleKey: "Shift+N"
    }
}
