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
            helpText: qsTr("Photos added recently, whatever their capture date")
            shown: pinhole.library.count > 0
            onTriggered: recent.open()
        }
    ]
    PluginView {
        id: recent
        source: "RecentPhotos.qml"
        toggleKey: "Shift+N"
    }
}
