import QtQuick

// One of a few, chosen on a row of buttons, each also its number's key (1,
// 2, 3…) while `when`: `options` as [{ value, text }], the one chosen
// `value`, and `chosen(value)` when another is. The keys are in the help
// under `section`, as `helpText`.
Row {
    id: choices
    property var options: []
    property var value
    property bool when: visible
    property string keyLayer: "plugin"
    property string section
    property string helpText
    property bool flat: true
    signal chosen(var value)

    Repeater {
        model: choices.options
        delegate: ShellButton {
            required property var modelData
            required property int index
            flat: choices.flat
            text: modelData.text
            hint: index < 9 ? String(index + 1) : ""
            active: choices.value === modelData.value
            onTapped: if (!active) choices.chosen(modelData.value)
        }
    }
    Repeater {
        model: Math.min(9, choices.options.length)
        delegate: Item {
            required property int index
            Command {
                name: "choices." + choices.helpText + "." + (index + 1)
                text: choices.helpText
                section: index === 0 ? choices.section : ""
                keys: [String(index + 1)]
                keysText: "1–" + Math.min(9, choices.options.length)
                keyHelp: index === 0 && choices.helpText !== ""
                keyLayer: choices.keyLayer
                when: choices.when
                mouse: qsTr("Its button")
                touchpad: qsTr("Its button")
                pointerHelp: false
                onTriggered: {
                    const option = choices.options[index]
                    if (option && option.value !== choices.value)
                        choices.chosen(option.value)
                }
            }
        }
    }
}
