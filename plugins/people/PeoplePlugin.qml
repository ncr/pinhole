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
           ["C", qsTr("Check the groups that look like the person shown")]]
    settings: Component {
        Column {
            width: parent ? parent.width : 0
            spacing: 6
            readonly property string grouping: pinhole.settings.grouping || "careful"
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: Math.round(theme.fontSize * 0.857)
                text: qsTr("How readily similar faces are put in one group. Fewer groups to name, or fewer mistakes in them.")
            }
            Flow {
                width: parent.width
                spacing: 4
                Repeater {
                    model: [["off", qsTr("Apart")], ["careful", qsTr("Careful")], ["wider", qsTr("Wider")]]
                    delegate: ShellButton {
                        required property var modelData
                        objectName: "peopleGrouping_" + modelData[0]
                        flat: true
                        checkable: true
                        text: modelData[1]
                        active: parent.parent.grouping === modelData[0]
                        onTapped: pinhole.settings.grouping = modelData[0]
                    }
                }
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
