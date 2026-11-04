import QtQuick
import Pinhole.Plugin

ScrollArea {
    id: editor
    required property string photo
    required property bool pending
    required property var suggestions
    required property var saved
    required property string errorText
    signal closed()
    signal toggled(var tag)

    contentWidth: width
    contentHeight: content.height + 32
    Component.onCompleted: back.forceActiveFocus()

    Column {
        id: content
        x: 16
        y: 16
        width: Math.max(0, editor.width - 32)
        spacing: 12

        ShellButton {
            id: back
            text: qsTr("Back to search")
            hint: "Esc"
            onTapped: editor.closed()
        }
        Image {
            width: parent.width
            height: 140
            source: editor.photo
            fillMode: Image.PreserveAspectFit
            asynchronous: true
        }
        UiText {
            width: parent.width
            text: qsTr("Saved tags")
            Accessible.role: Accessible.Heading
        }
        UiText {
            width: parent.width
            visible: editor.saved.length === 0
            text: qsTr("This photo has no saved tags.")
            wrapMode: Text.WordWrap
        }
        Flow {
            width: parent.width
            spacing: 8
            Repeater {
                model: editor.saved
                delegate: ShellButton {
                    required property var modelData
                    text: modelData.label + " ×"
                    Accessible.name: qsTr("Remove tag: %1").arg(modelData.label)
                    onTapped: {
                        editor.toggled(modelData)
                        back.forceActiveFocus()
                    }
                }
            }
        }
        UiText {
            width: parent.width
            text: qsTr("Suggested tags")
            Accessible.role: Accessible.Heading
        }
        UiText {
            width: parent.width
            wrapMode: Text.WordWrap
            text: editor.pending ? qsTr("Finding suggested tags…")
                : editor.errorText ? editor.errorText
                : editor.suggestions.length ? qsTr("Choose the suggestions that fit this photo.")
                : qsTr("Suggestions are unavailable for this photo.")
        }
        Flow {
            width: parent.width
            spacing: 8
            Repeater {
                model: editor.suggestions
                delegate: ShellButton {
                    required property var modelData
                    objectName: "semanticTagSuggestion"
                    text: modelData.label
                    active: editor.saved.some(tag => tag.id === modelData.id)
                    Accessible.name: (active ? qsTr("Remove tag: %1") : qsTr("Save tag: %1")).arg(modelData.label)
                    onTapped: editor.toggled(modelData)
                }
            }
        }
    }
}
