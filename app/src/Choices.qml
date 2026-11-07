import QtQuick

// One of a few, chosen on a row of buttons, each also its number's key (1,
// 2, 3…) while `usable`: `options` as [{ value, text }], the one chosen
// `value`, and `chosen(value)` when another is. The keys are in the help
// under `section`, as `helpText`. Given a `setting` (API 13), it shows and
// keeps that one of pinhole.settings, as a SettingSwitch does.
Row {
    id: choices
    // [{ value, text }], in order.
    property var options: []
    // The name of one of the plugin's settings, whose value it is.
    property string setting
    readonly property var _settings: setting !== "" && typeof pinhole !== "undefined" && pinhole ? pinhole.settings : null
    // The one chosen.
    property var value: _settings ? _settings[setting] : undefined
    // While true, the number keys choose.
    property bool usable: visible
    // Whose keys they are: the covering plugin's ("plugin"), or "grid" or
    // "viewer".
    property string keyLayer: "plugin"
    // Where the help lists the number keys, and in what words.
    property string section
    property string helpText
    property bool flat: true
    // Each its number's key (1, 2, 3…), shown on it; false for a setting's
    // row in the side panel, where the numbers are other things' keys.
    property bool numbered: true
    // Another chosen, with its value (`value` is not set by itself).
    signal chosen(var value)
    onChosen: value => { if (_settings) _settings[setting] = value }
    Accessible.role: Accessible.Grouping
    Accessible.name: helpText

    Repeater {
        model: choices.options
        delegate: ShellButton {
            required property var modelData
            required property int index
            flat: choices.flat
            text: modelData.text
            hint: choices.numbered && index < 9 ? String(index + 1) : ""
            active: choices.value === modelData.value
            checkable: true
            Accessible.role: Accessible.RadioButton
            onTriggered: if (!active) choices.chosen(modelData.value)
        }
    }
    Repeater {
        model: choices.numbered ? Math.min(9, choices.options.length) : 0
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
                usable: choices.usable
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
