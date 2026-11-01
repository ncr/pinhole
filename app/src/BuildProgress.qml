import QtQuick
import Pinhole

// A library being built, at the foot of the window while the grid fills
// above it: what is happening, how far it is, and about how long is left,
// from the pace so far.
Rectangle {
    id: card
    // What to call the source.
    property string name
    readonly property var progress: sources.progress
    readonly property string phase: progress.phase || ""
    readonly property double done: progress.done || 0
    readonly property double total: progress.total || 0

    visible: phase === "index" || phase === "thumbs"
    width: Math.min(parent.width - 32, theme.fontSize * 40)
    height: content.implicitHeight + 2 * Math.round(theme.fontSize)
    radius: Math.min(4, theme.radius)
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.94)
    border.width: 1
    border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.4)
    // Over the photos, not a hole in them.
    Shield { anchors.fill: parent; z: -1 }

    // The pace, from where this phase was first seen.
    property double startedAt: 0
    property double startedDone: 0
    property string startedPhase
    onDoneChanged: {
        if (phase !== startedPhase || done < startedDone) {
            startedPhase = phase
            startedAt = Date.now()
            startedDone = done
        }
    }
    readonly property string remaining: {
        const seconds = (Date.now() - startedAt) / 1000
        if (total <= 0 || seconds < 5 || done <= startedDone)
            return ""
        const minutes = Math.round((total - done) / ((done - startedDone) / seconds) / 60)
        return minutes < 1 ? qsTr("Under a minute left.") : qsTr("About %n minute(s) left.", "", minutes)
    }

    function count(n) { return Number(n).toLocaleString(Qt.locale(), "f", 0) }

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Math.round(theme.fontSize * 1.143)
        anchors.rightMargin: Math.round(theme.fontSize * 1.143)
        spacing: Math.round(theme.fontSize * 0.714)

        Item {
            width: parent.width
            height: title.implicitHeight
            UiText {
                id: title
                width: parent.width - counts.implicitWidth - theme.fontSize
                text: card.phase === "index" ? qsTr("Finding your photos in %1").arg(card.name)
                                             : qsTr("Making thumbnails")
                color: theme.brightForeground
                elide: Text.ElideNone
                wrapMode: Text.WordWrap
            }
            UiText {
                id: counts
                anchors.right: parent.right
                text: card.total > 0 ? qsTr("%1 of %2").arg(card.count(card.done)).arg(card.count(card.total))
                                     : card.count(card.done)
                font.features: { "tnum": 1 }
            }
        }
        Rectangle {
            width: parent.width
            height: Math.max(4, Math.round(theme.fontSize * 0.3))
            radius: height / 2
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
            Rectangle {
                width: parent.width * (card.total > 0 ? Math.min(1, card.done / card.total) : 0)
                height: parent.height
                radius: height / 2
                color: theme.accent
                Behavior on width { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
            }
        }
        UiText {
            width: parent.width
            wrapMode: Text.WordWrap
            color: theme.darkForeground
            text: (card.phase === "index"
                   ? qsTr("Newest first. Look around already: what you open comes straight from %1.").arg(card.name)
                   : qsTr("Photos sharpen as their thumbnails arrive."))
                  + (card.remaining !== "" ? " " + card.remaining : "")
        }
    }
}
