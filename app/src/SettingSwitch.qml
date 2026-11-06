import QtQuick

// A plugin's setting that is on or off (API 11): its words, and a switch
// at the right that says `toggled()`; with `usable` false, dimmed and
// still. Pressing the words presses the switch.
//
// Given a `setting` (API 13), it is that one of pinhole.settings: shown as
// it is (its default from the manifest's "settings" until chosen), and
// flipped and kept when pressed, with nothing more to write:
//
//     SettingSwitch { setting: "shuffle"; text: qsTr("Shuffle") }
Item {
    id: row
    property string text
    // The name of one of the plugin's settings, true or false.
    property string setting
    readonly property var _settings: setting !== "" && typeof pinhole !== "undefined" && pinhole ? pinhole.settings : null
    property bool checked: _settings ? _settings[setting] === true : false
    property bool usable: true
    // The switch's key, shown beside it ("⇧ S").
    property alias hint: toggle.hint
    // Pressed. It does not flip `checked` itself: set the setting, which
    // `checked` is bound to.
    signal toggled()
    onToggled: if (_settings) _settings[setting] = !checked
    width: parent ? parent.width : 0
    height: Math.max(toggle.implicitHeight, label.implicitHeight)
    UiText {
        id: label
        width: parent.width - toggle.width - theme.fontSize
        anchors.verticalCenter: parent.verticalCenter
        wrapMode: Text.WordWrap
        text: row.text
        color: row.usable ? theme.foreground : theme.darkForeground
        TapHandler { enabled: row.usable; onTapped: row.toggled() }
    }
    Toggle {
        id: toggle
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        Accessible.name: row.text
        usable: row.usable
        checked: row.checked
        onToggled: row.toggled()
    }
}
