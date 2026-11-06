import QtQuick
import Pinhole.Plugin

PinholePlugin {
    actions: [PluginAction {
        place: "grid"
        text: qsTr("People")
        key: "Shift+F"
        shown: pinhole.library.count > 0
        onTriggered: people.open()
    }]
    help: [["Shift+F", qsTr("People")], ["1–9", qsTr("Name the group shown")], ["X", qsTr("Take the photo out")],
           ["0", qsTr("Skip a face you do not know, for later")],
           ["C", qsTr("Check the groups that look like the person shown")]]
    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 6
            SettingNote {
                text: qsTr("How readily similar faces are put in one group. Fewer groups to name, or fewer mistakes in them.")
            }
            Choices {
                numbered: false
                setting: "grouping"
                options: [{ value: "off", text: qsTr("Apart") }, { value: "careful", text: qsTr("Careful") }, { value: "wider", text: qsTr("Wider") }]
            }
        }
    }
    PluginView { id: people; source: "PeoplePanel.qml"; toggleKey: "Shift+F" }
}
