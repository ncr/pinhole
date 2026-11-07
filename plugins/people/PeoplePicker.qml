import QtQuick
import Pinhole.Plugin

FocusScope {
    id: picker
    required property var names
    required property string title
    required property bool busy
    required property bool allowCreate
    // Who it most likely is: Yes, and ⏎ with nothing typed, choose them.
    property string suggested: ""
    // A button that takes the name away ("Not Anna"), when said.
    property string removeText: ""
    property alias searchField: search.input
    property alias createButton: create
    property alias cancelButton: cancel
    property alias yesButton: yes
    property alias removeButton: remove
    property alias choicesList: list
    readonly property string typedName: search.text.trim()
    readonly property var matches: names.filter(name => name.toLowerCase().includes(typedName.toLowerCase()))
    readonly property bool nameExists: names.some(name => name.toLowerCase() === typedName.toLowerCase())
    signal chosen(string name)
    signal cancelled()
    signal removed()

    function reset() {
        search.text = ""
        list.currentIndex = -1
        search.input.forceActiveFocus()
    }

    Column {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 8
        UiText { width: parent.width; wrapMode: Text.WordWrap; text: picker.title }
        FieldInput {
            id: search
            objectName: "peoplePickerSearch"
            width: parent.width
            placeholder: picker.allowCreate ? qsTr("Find a person or enter a new name") : qsTr("Find a person")
            input.maximumLength: 100
            onTextChanged: list.currentIndex = -1
            onAccepted: {
                if (picker.busy) return
                if (picker.typedName === "" && picker.suggested !== "") {
                    picker.chosen(picker.suggested)
                    return
                }
                const existing = picker.names.find(name => name.toLowerCase() === picker.typedName.toLowerCase())
                if (existing) {
                    picker.chosen(existing)
                } else if (list.count) {
                    list.currentIndex = 0
                    list.forceActiveFocus()
                } else if (create.usable) {
                    picker.chosen(picker.typedName)
                }
            }
        }
    }
    ScrollList {
        id: list
        objectName: "peoplePickerList"
        anchors.top: header.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: actions.top
        anchors.bottomMargin: 8
        model: picker.matches
        currentIndex: -1
        activeFocusOnTab: count > 0 && !picker.busy
        Accessible.name: qsTr("Existing people")
        Keys.onReturnPressed: { if (!picker.busy && currentIndex >= 0) picker.chosen(picker.matches[currentIndex]) }
        Keys.onEnterPressed: { if (!picker.busy && currentIndex >= 0) picker.chosen(picker.matches[currentIndex]) }
        Keys.onSpacePressed: { if (!picker.busy && currentIndex >= 0) picker.chosen(picker.matches[currentIndex]) }
        Keys.onDownPressed: currentIndex = Math.min(count - 1, currentIndex + 1)
        Keys.onUpPressed: if (count) currentIndex = Math.max(0, currentIndex - 1)
        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
        delegate: ShellButton {
            required property string modelData
            required property int index
            objectName: "peoplePickerChoice"
            width: list.width
            flat: true
            justified: true
            text: modelData
            usable: !picker.busy
            activeFocusOnTab: false
            lit: list.activeFocus && list.currentIndex === index
            onTriggered: picker.chosen(modelData)
        }
    }
    Flow {
        id: actions
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 8
        ShellButton {
            id: yes
            objectName: "peopleYes"
            text: qsTr("Yes, %1").arg(picker.suggested)
            hint: "⏎"
            visible: picker.suggested !== "" && picker.typedName === ""
            active: true
            usable: visible && !picker.busy
            onTriggered: picker.chosen(picker.suggested)
        }
        ShellButton {
            id: create
            objectName: "peopleCreate"
            text: qsTr("Add a new person")
            visible: picker.allowCreate
            usable: visible && !picker.busy && picker.typedName !== "" && !picker.nameExists
            onTriggered: picker.chosen(picker.typedName)
        }
        ShellButton {
            id: remove
            objectName: "peopleRemove"
            text: picker.removeText
            visible: picker.removeText !== ""
            usable: visible && !picker.busy
            onTriggered: picker.removed()
        }
        ShellButton { id: cancel; text: qsTr("Cancel"); onTriggered: picker.cancelled() }
    }
}
