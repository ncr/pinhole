import QtQuick
import Pinhole.Plugin

PinholePlugin {
    covering: view.item?.shown === true
    actions: [PluginAction {
        place: "grid"
        text: qsTr("People")
        key: "Shift+F"
        shown: pinhole.library.count > 0
        onTriggered: {
            view.active = true
            view.item.show()
        }
    }]
    Loader {
        id: view
        anchors.fill: parent
        active: false
        sourceComponent: PeoplePanel {}
    }
}
