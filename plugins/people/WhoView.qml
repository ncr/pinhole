import QtQuick
import QtQuick.Window
import Pinhole.Plugin

// Who is in the photo shown (W, or its button, Main.qml): each face
// ringed on the photo, the one being named in the accent, and beside it a
// card to say who that is: their name, a new one, Yes to the person it
// looks like, or Not to the name it has. ⏎ goes on to the next face not
// named yet, and closes after the last. A ring or a face on the card
// chooses another. Each is a step Ctrl Z undoes.
FocusScope {
    id: who
    objectName: "peopleWhoView"
    readonly property var plugin: view.plugin
    readonly property var faces: plugin.faces
    property string chosenKey: ""
    readonly property int at: faces.findIndex(face => face.key === chosenKey)
    readonly property var face: at >= 0 ? faces[at] : null
    // Where the photo is on the window, as it was when the card opened.
    property rect frame: Qt.rect(0, 0, width, height)

    function start() {
        const first = faces.findIndex(face => !face.name)
        choose(first >= 0 ? first : 0)
        Qt.callLater(measure)
    }
    // Its field lets go of the keys as it closes: the next goes to the
    // photo (W again, an arrow).
    function stop() {
        chosenKey = ""
        picker.searchField.focus = false
        who.focus = false
    }
    function measure() {
        const layer = plugin.photoFrame
        if (layer) frame = layer.mapToItem(who, 0, 0, layer.width, layer.height)
    }
    onWidthChanged: Qt.callLater(measure)
    onHeightChanged: Qt.callLater(measure)
    function choose(index) {
        if (index < 0 || index >= faces.length) return
        chosenKey = faces[index].key
        picker.reset()
    }
    // On to the next face not named yet, after this one; none: done.
    function onwards() {
        for (let step = 1; step < faces.length; ++step) {
            const next = (at + step) % faces.length
            if (!faces[next].name) {
                choose(next)
                return
            }
        }
        view.close()
    }
    function named(name) {
        const shown = face
        if (!shown) return
        if (name !== shown.name) plugin.nameFace(shown, name)
        onwards()
    }
    // Another photo, or none: this card was about that one.
    Connections {
        target: who.plugin
        function onShownIdChanged() { if (view.shown) view.close() }
        function onFacesChanged() {
            if (view.shown && who.faces.length === 0) view.close()
        }
    }
    // A face's ring on the photo, a little larger than the face.
    function ring(face) {
        const d = face.drawn
        const side = Math.max(d[2] * frame.width, d[3] * frame.height) * 1.3
        return Qt.rect(frame.x + (d[0] + d[2] / 2) * frame.width - side / 2,
                       frame.y + (d[1] + d[3] / 2) * frame.height - side / 2, side, side)
    }

    // Over the photo, a little dimmed; a click beside the card closes it.
    Panel {
        color: Qt.rgba(0, 0, 0, 0.3)
        onTriggered: view.close()
    }
    Repeater {
        model: who.faces
        delegate: Item {
            id: mark
            required property var modelData
            required property int index
            readonly property rect at: who.ring(modelData)
            readonly property bool chosen: modelData.key === who.chosenKey
            x: at.x
            y: at.y
            width: at.width
            height: at.height
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "transparent"
                border.width: mark.chosen ? 3 : 1.5
                border.color: mark.chosen ? theme.accent : Qt.rgba(1, 1, 1, ringHover.hovered ? 0.95 : 0.7)
            }
            // Its name under it, or who it may be.
            Rectangle {
                anchors.top: parent.bottom
                anchors.topMargin: 6
                anchors.horizontalCenter: parent.horizontalCenter
                width: label.implicitWidth + 12
                height: label.implicitHeight + 4
                radius: Math.min(4, theme.radius)
                color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.85)
                UiText {
                    id: label
                    anchors.centerIn: parent
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                    color: mark.chosen ? theme.accent : mark.modelData.name ? theme.brightForeground : theme.darkForeground
                    text: mark.modelData.name || (mark.modelData.guess ? qsTr("%1?").arg(mark.modelData.guess) : "?")
                }
            }
            HoverHandler { id: ringHover; cursorShape: Qt.PointingHandCursor }
            // Its own, not the dimmed panel's beneath (which closes).
            TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: who.choose(mark.index) }
        }
    }

    // On the side away from the face being named.
    Card {
        id: card
        objectName: "peopleWhoCard"
        readonly property int padding: 16
        readonly property bool leftSide: who.face ? who.ring(who.face).x + who.ring(who.face).width / 2 > who.width / 2 : false
        width: Math.min(who.width - 32, theme.fontSize * 28)
        height: Math.min(who.height - 140, theme.fontSize * 32)
        x: leftSide ? 24 : who.width - width - 24
        y: Math.max(16, (who.height - height) / 2)
        Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

        Flow {
            id: tiles
            x: card.padding
            y: card.padding
            width: card.width - 2 * card.padding
            spacing: 10
            Repeater {
                model: who.faces
                delegate: PeopleTile {
                    required property var modelData
                    required property int index
                    objectName: "peopleWhoFace"
                    width: Math.round(theme.fontSize * (who.faces.length > 1 ? 3.2 : 4.4))
                    height: width
                    row: modelData
                    chosen: modelData.key === who.chosenKey
                    activeFocusOnTab: false
                    Accessible.name: modelData.name || (modelData.guess ? qsTr("%1?").arg(modelData.guess) : qsTr("Face %1").arg(index + 1))
                    onSelected: who.choose(index)
                }
            }
        }
        PeoplePicker {
            id: picker
            x: card.padding
            width: card.width - 2 * card.padding
            anchors.top: tiles.bottom
            anchors.topMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: card.padding
            focus: true
            readonly property var face: who.face
            names: who.plugin.names.slice().sort((a, b) => (b === face?.guess) - (a === face?.guess))
            busy: false
            allowCreate: true
            suggested: face && !face.name && face.guess ? face.guess : ""
            removeText: face && who.plugin.canUnname(face) ? qsTr("Not %1").arg(face.name) : ""
            title: !face ? "" : face.name ? qsTr("This is %1. If not, who is it?").arg(face.name)
                 : face.guess ? qsTr("Is this %1?").arg(face.guess) : qsTr("Who is this?")
            onChosen: name => who.named(name)
            onRemoved: {
                who.plugin.unnameFace(face)
                picker.reset()
            }
            onCancelled: view.close()
        }
    }

    // Tab and ⇧ Tab go round the card's controls (Pinhole's keys would
    // take them otherwise).
    function tabKey(event) {
        return (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
    }
    Keys.onShortcutOverride: event => { if (tabKey(event)) event.accepted = true }
    Keys.onPressed: event => {
        if (!tabKey(event)) return
        const controls = [picker.searchField, picker.choicesList, picker.yesButton, picker.createButton,
                          picker.removeButton, picker.cancelButton]
            .filter(item => item && item.visible && item.activeFocusOnTab !== false && item.usable !== false)
        const reverse = event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier)
        const current = controls.indexOf(who.Window.window.activeFocusItem)
        const index = current < 0 ? (reverse ? controls.length - 1 : 0)
            : (current + (reverse ? -1 : 1) + controls.length) % controls.length
        controls[index]?.forceActiveFocus(reverse ? Qt.BacktabFocusReason : Qt.TabFocusReason)
        event.accepted = true
    }
}
