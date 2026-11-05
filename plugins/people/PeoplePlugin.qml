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
    help: [["Shift+F", qsTr("People")], ["1–9", qsTr("Name the group shown")], ["X", qsTr("Take the photo out")],
           ["0", qsTr("Skip a face you do not know, for later")],
           ["C", qsTr("Check the groups that look like the person shown")]]
    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 6
            readonly property string grouping: pinhole.settings.grouping || "careful"
            SettingNote {
                text: qsTr("How readily similar faces are put in one group. Fewer groups to name, or fewer mistakes in them.")
            }
            Choices {
                numbered: false
                options: [{ value: "off", text: qsTr("Apart") }, { value: "careful", text: qsTr("Careful") }, { value: "wider", text: qsTr("Wider") }]
                value: parent.grouping
                onChosen: value => pinhole.settings.grouping = value
            }
        }
    }
    Loader {
        id: view
        anchors.fill: parent
        active: false
        sourceComponent: PeoplePanel {}
    }
}
