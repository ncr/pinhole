import QtQuick

// Pinhole's settings, on the gear button or ",": where the photos come from
// (iCloud, or a folder on this computer), the one folder it keeps everything
// in (the library, and the photos fetched from the source), how much room
// that takes and how much it may; the library always fits. As an Omarchy
// panel: a scrim, a card with a hairline, keys beside what they press.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false
    onShownChanged: {
        storage.watching = shown
        pending = -1
    }

    // A limit stepped to but not yet set. One at or above what is kept is
    // set a moment later; one below it waits to be confirmed, since setting
    // it removes photos.
    property double pending: -1
    readonly property double chosen: pending >= 0 ? pending : storage.limit
    readonly property bool confirming: pending >= 0 && pending < storage.used
    readonly property double removing: Math.max(0, storage.used - chosen)

    // The limits to step through: the least (the library and a little room
    // for photos), round sizes up to what this disk could hold, the default
    // and the one set now.
    readonly property var steps: {
        const gb = 1024 * 1024 * 1024
        const most = Math.max(storage.limit, storage.used + storage.free)
        const list = [5, 10, 20, 50, 100, 200, 500, 1000, 2000]
            .map(n => n * gb).filter(b => b > storage.minLimit && b <= most)
        for (const b of [storage.minLimit, storage.defaultLimit, storage.limit])
            if (b > 0 && list.indexOf(b) < 0)
                list.push(b)
        return list.sort((a, b) => a - b)
    }
    function step(direction) {
        if (steps.length === 0)
            return
        let at = 0
        while (at < steps.length - 1 && steps[at] < chosen)
            ++at
        if (steps[at] > chosen && direction > 0)
            --at
        const next = steps[Math.max(0, Math.min(steps.length - 1, at + direction))]
        pending = next === storage.limit ? -1 : next
        if (pending >= 0 && !confirming)
            settle.restart()
    }
    function confirm() {
        if (pending >= 0)
            storage.setLimit(pending)
        pending = -1
    }
    // Stepping up (or down while still above what is kept) sets the limit
    // once the steps stop.
    Timer { id: settle; interval: 600; onTriggered: if (!root.confirming) root.confirm() }

    readonly property var kindNames: ({
        "library": qsTr("Index and thumbnails"),
        "full": qsTr("Full size"),
        "medium": qsTr("Previews"),
        "video": qsTr("Videos"),
        "original": qsTr("Originals"),
        "live": qsTr("Live Photos")
    })

    Shortcut { enabled: root.shown && storage.loaded; sequence: "Left"; context: Qt.ApplicationShortcut; onActivated: root.step(-1) }
    Shortcut { enabled: root.shown && storage.loaded; sequence: "Right"; context: Qt.ApplicationShortcut; onActivated: root.step(1) }
    Shortcut { enabled: root.shown && root.confirming; sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; onActivated: root.confirm() }
    Shortcut {
        enabled: root.shown; sequence: "Escape"; context: Qt.ApplicationShortcut
        onActivated: root.confirming ? root.pending = -1 : root.shown = false
    }
    Shortcut { enabled: root.shown && storage.outdated; sequence: "R"; context: Qt.ApplicationShortcut; onActivated: storage.restartService() }
    Shortcut { enabled: root.shown && storage.loaded && !storage.moving; sequence: "L"; context: Qt.ApplicationShortcut; onActivated: storage.chooseLocation() }
    Shortcut { enabled: root.shown && storage.sourceKind !== "icloud"; sequence: "I"; context: Qt.ApplicationShortcut; onActivated: storage.useICloud() }
    Shortcut { enabled: root.shown; sequence: "P"; context: Qt.ApplicationShortcut; onActivated: storage.chooseSourceFolder() }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
    }
    MouseArea {
        anchors.fill: parent
        onClicked: root.shown = false
    }

    Rectangle {
        id: card
        readonly property int padding: 2 * theme.fontSize
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, theme.fontSize * 36)
        height: Math.min(parent.height - 32, content.implicitHeight + 2 * padding)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        clip: true
        // Clicks inside stay inside.
        MouseArea { anchors.fill: parent }

        Flickable {
            anchors.fill: parent
            anchors.margins: card.padding
            contentHeight: content.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: content
                width: parent.width
                spacing: theme.fontSize

                Item {
                    width: parent.width
                    height: close.height
                    UiText {
                        y: centredIn(parent.height)
                        text: qsTr("Settings")
                        color: theme.accent
                    }
                    ShellButton {
                        id: close
                        anchors.right: parent.right
                        flat: true
                        text: "✕"
                        hint: "Esc"
                        onTapped: root.shown = false
                    }
                }

                // Where the photos come from: iCloud, or a folder, each on a
                // button of its own; the one in use is lit.
                Column {
                    width: parent.width
                    spacing: 4
                    UiText {
                        text: qsTr("Photos from")
                        color: theme.darkForeground
                    }
                    Item {
                        width: parent.width
                        height: icloudButton.height
                        UiText {
                            width: parent.width - sourceButtons.width - theme.fontSize
                            y: centredIn(parent.height)
                            text: storage.sourceKind === "folder" ? storage.sourcePath : storage.sourceName
                            color: theme.brightForeground
                            elide: Text.ElideMiddle
                        }
                        Row {
                            id: sourceButtons
                            anchors.right: parent.right
                            spacing: 6
                            ShellButton {
                                id: icloudButton
                                active: storage.sourceKind === "icloud"
                                text: "iCloud"
                                hint: "I"
                                onTapped: storage.useICloud()
                            }
                            ShellButton {
                                active: storage.sourceKind === "folder"
                                text: qsTr("Folder")
                                hint: "P"
                                onTapped: storage.chooseSourceFolder()
                            }
                        }
                    }
                }

                // Where.
                Column {
                    visible: storage.loaded
                    width: parent.width
                    spacing: 4
                    UiText {
                        text: qsTr("Pinhole keeps its library and photos in")
                        color: theme.darkForeground
                    }
                    Item {
                        width: parent.width
                        height: change.height
                        UiText {
                            width: parent.width - change.width - theme.fontSize
                            y: centredIn(parent.height)
                            text: storage.location
                            color: theme.brightForeground
                            elide: Text.ElideMiddle
                        }
                        ShellButton {
                            id: change
                            anchors.right: parent.right
                            usable: !storage.moving
                            text: qsTr("Change")
                            hint: "L"
                            onTapped: storage.chooseLocation()
                        }
                    }
                }

                // How much: what is kept against the limit, in the accent;
                // what a lower limit would remove, in red.
                Column {
                    visible: storage.loaded
                    width: parent.width
                    spacing: 6
                    Item {
                        width: parent.width
                        height: Math.max(4, Math.round(theme.fontSize * 0.4))
                        readonly property double whole: Math.max(root.chosen, storage.used, 1)
                        Rectangle {
                            anchors.fill: parent
                            radius: height / 2
                            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                        }
                        Rectangle {
                            width: parent.width * Math.min(storage.used, root.chosen) / parent.whole
                            height: parent.height
                            radius: height / 2
                            color: theme.accent
                            Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                        }
                        Rectangle {
                            visible: root.removing > 0
                            x: parent.width * root.chosen / parent.whole
                            width: parent.width - x
                            height: parent.height
                            radius: height / 2
                            color: theme.red
                        }
                    }
                    UiText {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: qsTr("%1 of %2 used · %3 free on this disk")
                              .arg(storage.size(storage.used)).arg(storage.size(storage.limit)).arg(storage.size(storage.free))
                    }
                    UiText {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: theme.darkForeground
                        text: {
                            const parts = []
                            for (const kind of ["library", "full", "medium", "video", "original", "live"]) {
                                const bytes = storage.kinds[kind] || 0
                                if (bytes > 0)
                                    // Kept on one line each.
                                    parts.push((root.kindNames[kind] + " " + storage.size(bytes)).replace(/ /g, "\u00a0"))
                            }
                            return parts.join(" · ")
                        }
                        visible: text !== ""
                    }
                }

                // The limit, stepped with the buttons or ← →.
                Item {
                    visible: storage.loaded
                    width: parent.width
                    height: less.height
                    UiText {
                        y: centredIn(parent.height)
                        text: qsTr("Keep at most")
                    }
                    Row {
                        anchors.right: parent.right
                        spacing: 6
                        ShellButton {
                            id: less
                            text: "−"
                            hint: "←"
                            usable: root.steps.length > 0 && root.chosen > root.steps[0]
                            onTapped: root.step(-1)
                        }
                        UiText {
                            width: Math.max(implicitWidth, theme.fontSize * 5)
                            height: less.height
                            y: 0
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: storage.size(root.chosen)
                            color: root.confirming ? theme.red : theme.brightForeground
                        }
                        ShellButton {
                            text: "+"
                            hint: "→"
                            usable: root.steps.length > 0 && root.chosen < root.steps[root.steps.length - 1]
                            onTapped: root.step(1)
                        }
                    }
                }

                // A lower limit than what is kept: what it removes, asked first.
                Column {
                    visible: root.confirming
                    width: parent.width
                    spacing: theme.fontSize
                    UiText {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: theme.red
                        text: qsTr("This removes %1 of the photos viewed longest ago from this computer. They stay in %2 and come back when you look at them.")
                              .arg(storage.size(root.removing)).arg(storage.sourceName)
                    }
                    Row {
                        spacing: 6
                        ShellButton {
                            active: true
                            text: qsTr("Remove %1").arg(storage.size(root.removing))
                            hint: "Enter"
                            onTapped: root.confirm()
                        }
                        ShellButton {
                            text: qsTr("Cancel")
                            hint: "Esc"
                            onTapped: root.pending = -1
                        }
                    }
                }

                // Under way.
                Row {
                    visible: storage.freeing
                    spacing: theme.fontSize
                    Spinner { busy: storage.freeing; delay: 0 }
                    UiText {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Freeing space…")
                    }
                }
                Column {
                    visible: storage.moving
                    width: parent.width
                    spacing: 6
                    UiText {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: qsTr("Moving everything to the new folder: %1 of %2")
                              .arg(storage.size(storage.moved)).arg(storage.size(storage.moveTotal))
                    }
                    Item {
                        width: parent.width
                        height: Math.max(4, Math.round(theme.fontSize * 0.4))
                        Rectangle {
                            anchors.fill: parent
                            radius: height / 2
                            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                        }
                        Rectangle {
                            width: parent.width * (storage.moveTotal > 0 ? storage.moved / storage.moveTotal : 0)
                            height: parent.height
                            radius: height / 2
                            color: theme.accent
                        }
                    }
                }

                UiText {
                    visible: storage.error !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: theme.red
                    text: storage.error
                }
                ShellButton {
                    visible: storage.outdated
                    text: qsTr("Restart it")
                    hint: "R"
                    onTapped: storage.restartService()
                }

                // What the limit can and cannot do.
                UiText {
                    visible: storage.loaded
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: theme.darkForeground
                    text: qsTr("The index and thumbnails always stay, so the limit is never under %1. Unless you choose one, it is %2.")
                          .arg(storage.size(storage.minLimit)).arg(storage.size(storage.defaultLimit))
                }
            }
        }
    }
}
