import QtQuick

// More than one plugin whose tour has not been seen (a first launch, or a
// few new at once): one card listing them, each with a line of what it
// does and Show for its tour, and Later, in place of one tour after
// another. However it closes, they all count as seen; each tour stays in
// the side panel. ↑ ↓ choose, ⏎ shows, Esc is Later.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160 } }

    property bool shown: false
    // The plugins listed, by id; and which tours can be shown yet (`tourReady`).
    property var ids: []
    property var ready: id => true
    // The row chosen with the keys.
    property int current: 0
    signal tourRequested(string id)

    function open(list) {
        ids = list
        current = 0
        shown = true
    }
    function close() {
        if (!shown)
            return
        shown = false
        for (const id of ids)
            plugins.setToured(id)
    }
    function show(id) {
        close()
        tourRequested(id)
    }

    // The window dimmed under it, as under a panel; a click beside it is
    // Later.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onClicked: root.close()
        onWheel: wheel => wheel.accepted = true
    }

    // The heading and Later stay; the plugins between scroll, in a window
    // too short for them all.
    Rectangle {
        id: card
        objectName: "newPluginsCard"
        anchors.centerIn: parent
        readonly property int padding: Math.round(theme.fontSize * 1.714)
        readonly property int gap: Math.round(theme.fontSize * 0.8)
        width: Math.min(parent.width - 32, Math.round(theme.fontSize * 40))
        height: Math.min(parent.height - 32, 2 * padding + head.height + 2 * gap + list.implicitHeight + later.height)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        clip: true
        MouseArea { anchors.fill: parent }

        Column {
            id: head
            x: card.padding
            y: card.padding
            width: card.width - 2 * card.padding
            spacing: Math.round(theme.fontSize * 0.6)
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("New plugins")
                color: theme.brightForeground
                font.pixelSize: Math.round(theme.fontSize * 1.286)
            }
            UiText {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Each has a short tour of what it does; they stay in the side panel (Tab).")
                color: theme.darkForeground
            }
        }
        Flickable {
            id: scroller
            x: card.padding
            y: head.y + head.height + card.gap
            width: head.width
            height: later.y - card.gap - y
            contentHeight: list.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Column {
                id: list
                width: scroller.width
                spacing: Math.round(theme.fontSize * 0.3)
                Repeater {
                    id: rows
                    model: root.ids
                    delegate: Rectangle {
                        id: row
                        required property string modelData
                        required property int index
                        readonly property var info: (plugins.revision, plugins.at(modelData))
                        readonly property bool chosen: root.current === index
                        width: list.width
                        height: Math.max(words.implicitHeight, showButton.height) + 2 * inset
                        readonly property int inset: Math.round(theme.fontSize * 0.4)
                        radius: Math.min(4, theme.radius)
                        color: chosen && keymap.input === "keyboard"
                               ? Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.06) : "transparent"
                        // Chosen with the keys: in sight.
                        onChosenChanged: if (chosen) root.keepInSight(row)
                        Column {
                            id: words
                            x: row.inset
                            anchors.verticalCenter: parent.verticalCenter
                            width: showButton.x - x - theme.fontSize
                            spacing: 2
                            UiText {
                                width: parent.width
                                elide: Text.ElideRight
                                text: row.info.name ?? row.modelData
                                color: theme.brightForeground
                            }
                            UiText {
                                width: parent.width
                                elide: Text.ElideRight
                                text: row.info.description ?? ""
                                color: theme.darkForeground
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                        }
                        ShellButton {
                            id: showButton
                            anchors.right: parent.right
                            anchors.rightMargin: row.inset
                            anchors.verticalCenter: parent.verticalCenter
                            flat: !row.chosen
                            text: qsTr("Show")
                            hint: row.chosen && keymap.input === "keyboard" ? "⏎" : ""
                            usable: root.ready(row.modelData)
                            onTapped: root.show(row.modelData)
                        }
                    }
                }
            }
        }
        ShellButton {
            id: later
            anchors.right: parent.right
            anchors.rightMargin: card.padding
            y: card.height - card.padding - height
            text: qsTr("Later")
            action: laterAction
        }
    }
    function keepInSight(row) {
        const y = row.mapToItem(list, 0, 0).y
        if (y < scroller.contentY)
            scroller.contentY = y
        else if (y + row.height > scroller.contentY + scroller.height)
            scroller.contentY = y + row.height - scroller.height
    }


    // Its keys, while it is on top (Command.qml).
    Command {
        name: "newPlugins.up"
        keys: ["Up"]
        keyLayer: "newPlugins"
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: root.current = Math.max(0, root.current - 1)
    }
    Command {
        name: "newPlugins.down"
        keys: ["Down", "Tab"]
        keyLayer: "newPlugins"
        autoRepeat: true
        mouse: qsTr("Point")
        touchpad: qsTr("Point")
        onTriggered: root.current = Math.min(root.ids.length - 1, root.current + 1)
    }
    Command {
        name: "newPlugins.show"
        keys: ["Return", "Enter"]
        keyLayer: "newPlugins"
        when: root.current >= 0 && root.current < root.ids.length && root.ready(root.ids[root.current])
        mouse: qsTr("Show")
        touchpad: qsTr("Show")
        onTriggered: root.show(root.ids[root.current])
    }
    Command {
        id: laterAction
        name: "newPlugins.later"
        keys: ["Escape"]
        keyLayer: "newPlugins"
        onTriggered: root.close()
    }
}
