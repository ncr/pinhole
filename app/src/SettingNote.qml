import QtQuick

// A plugin's settings' few words: what a choice does, or how it
// is now, in the side panel's smaller type, wrapped; `dim` for a remark.
UiText {
    property bool dim: false
    width: parent ? parent.width : 0
    wrapMode: Text.WordWrap
    font.pixelSize: Math.round(theme.fontSize * 0.857)
    color: dim ? theme.darkForeground : theme.foreground
}
