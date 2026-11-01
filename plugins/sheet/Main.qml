import QtQuick
import Pinhole.Plugin

// Contact sheet: the photos ticked in select mode made one picture to
// send, an even grid of them or a collage that keeps each whole
// (SheetStudio.qml). magick makes it from the photos at the size it
// needs, and it is copied and kept in Pictures/Pinhole, as Series keeps
// its clips.
PinholePlugin {
    id: plugin
    covering: studio.shown

    // The theme's font as a file, for magick to write the dates in.
    property string fontFile: ""
    Component.onCompleted: pinhole.run("fc-match", ["-f", "%{file}", theme.font])
        .then(result => fontFile = result.stdout.trim()).catch(pinhole.warn)

    actions: [
        PluginAction {
            id: sheetAction
            place: "selection"
            text: qsTr("Sheet")
            key: "K"
            usable: pinhole.selectionCount >= 2
            onTriggered: studio.open(pinhole.selection())
        }
    ]

    tour: [
        {
            title: qsTr("Several photos as one"),
            text: qsTr("In select mode, tick a few photos, then K (Sheet) makes them one picture to send."),
            photo: () => pinhole.currentIndex()
        },
        {
            title: qsTr("Grid or collage"),
            text: qsTr("An even grid, like a contact sheet, or a collage that keeps each photo whole; with a gap, a background, a size and the dates if you like. Enter copies it and keeps it in Pictures/Pinhole.")
        },
        {
            title: qsTr("Settings"),
            text: qsTr("Here, in the side panel (Tab), this tour can be shown again."),
            settings: true
        }
    ]

    help: [
        ["K", qsTr("One picture of what is ticked")]
    ]

    settings: Component {
        UiText {
            width: parent ? parent.width : 0
            wrapMode: Text.WordWrap
            color: theme.foreground
            font.pixelSize: Math.round(theme.fontSize * 0.857)
            text: qsTr("In select mode, tick two photos or more, then K.")
        }
    }

    SheetStudio {
        id: studio
        objectName: "sheetStudio"
        anchors.fill: parent
        plugin: plugin
    }
}
