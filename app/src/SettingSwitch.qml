import QtQuick

// A plugin's setting that is on or off (API 11): its words, and a switch
// at the right that says `toggled()`; with `usable` false, dimmed and
// still. Pressing the words presses the switch.
Item {
    id: row
    property string text
    property bool checked: false
    property bool usable: true
    // The switch's key, shown beside it ("⇧ S").
    property alias hint: toggle.hint
    // Pressed. It does not flip `checked` itself: set the setting, which
    // `checked` is bound to.
    signal toggled()
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
