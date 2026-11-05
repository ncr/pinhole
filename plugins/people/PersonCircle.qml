import QtQuick
import Pinhole.Plugin

// A person or a group at home: a round face, its name or who it may be,
// and how many photos.
Item {
    id: person
    required property var row
    property string caption
    property string detail
    property bool guessed: false
    property bool chosen: false
    signal selected()
    height: width + theme.fontSize * 2.8
    Accessible.role: Accessible.Button
    Accessible.name: caption + (detail ? ", " + detail : "")
    Accessible.onPressAction: selected()
    PeopleTile {
        id: face
        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        width: person.width - 28
        height: width
        row: person.row
        chosen: person.chosen
        onSelected: person.selected()
    }
    UiText {
        id: name
        anchors.top: face.bottom
        anchors.topMargin: 8
        x: 4
        width: parent.width - 8
        horizontalAlignment: Text.AlignHCenter
        // A long name in smaller type, never cut short.
        fontSizeMode: Text.HorizontalFit
        minimumPixelSize: Math.round(theme.fontSize * 0.7)
        font.pixelSize: theme.fontSize
        color: person.guessed ? theme.accent : face.hovered || person.chosen ? theme.brightForeground : theme.foreground
        text: person.caption
    }
    UiText {
        anchors.top: name.bottom
        x: 4
        width: parent.width - 8
        visible: person.detail !== ""
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        font.pixelSize: Math.round(theme.fontSize * 0.857)
        color: theme.darkForeground
        text: person.detail
    }
}
