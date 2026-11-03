import QtQuick
import Pinhole.Plugin

// No library scans or model processes on the first-frame path.
PinholePlugin {
    id: plugin
    covering: view.item !== null && view.item.shown
    actions: [PluginAction {
        place: "grid"
        text: qsTr("Search photos")
        key: "Ctrl+F"
        shown: pinhole.library.count > 0
        onTriggered: {
            view.active = true
            view.item.show()
        }
    }]
    help: [["Ctrl+F", qsTr("Search photo content")]]
    Loader {
        id: view
        anchors.fill: parent
        active: false
        sourceComponent: SemanticPanel {}
    }
}
