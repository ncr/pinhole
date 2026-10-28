import QtQuick

// Photos and videos dragged in from elsewhere (a file manager, say): the
// whole window takes them, lit in the accent while they hover, and adds
// them to the source shown (incoming). Only a drag that brings files the
// source takes; never photos being dragged out of this window.
DropArea {
    id: target
    // What to call the source.
    property string name
    // Files and folders the drag would add; neither when it is not for here.
    property int files: 0
    property int folders: 0
    readonly property bool lit: containsDrag && files + folders > 0

    keys: ["text/uri-list"]
    onEntered: drag => {
        const wanted = outgoing.dragging ? { files: 0, folders: 0 } : incoming.wanted(drag.urls)
        target.files = wanted.files
        target.folders = wanted.folders
        // Copied, always: a file manager must never take it as a move.
        if (target.files + target.folders > 0)
            drag.accept(Qt.CopyAction)
        else
            drag.accepted = false
    }
    onExited: target.files = target.folders = 0
    onDropped: drop => {
        if (target.files + target.folders > 0) {
            drop.accept(Qt.CopyAction)
            incoming.add(drop.urls)
        }
        target.files = target.folders = 0
    }

    Rectangle {
        anchors.fill: parent
        opacity: target.lit ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.72)

        Rectangle {
            anchors.fill: parent
            anchors.margins: 12
            radius: Math.min(8, theme.radius)
            color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)
            border.width: 2
            border.color: theme.accent
        }
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 64, theme.fontSize * 36)
            spacing: Math.round(theme.fontSize * 0.5)
            UiText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: target.folders > 0 ? qsTr("Add the photos and videos to %1").arg(target.name)
                                         : qsTr("Add %n file(s) to %1", "", target.files).arg(target.name)
                color: theme.brightForeground
                font.pixelSize: Math.round(theme.fontSize * 1.5)
            }
            UiText {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: storage.sourceKind === "icloud"
                      ? qsTr("Uploaded to your iCloud Photos. The files here stay as they are.")
                      : qsTr("Copied into the folder. The files here stay as they are.")
            }
        }
    }
}
