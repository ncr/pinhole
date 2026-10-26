import QtQuick

// Every source added, on Tab: over the photos from the left, hidden
// otherwise. One click or key shows another: ↑ ↓ and Enter here, 1 to 9
// from anywhere. What needs doing shows on its row (signing in again, a
// disk to connect, thumbnails still coming), and Delete takes one out,
// asked on the row first.
Item {
    id: root
    anchors.fill: parent
    visible: panel.x > -panel.width

    property bool shown: false
    // How the source shown is doing (the service's health).
    property string health: "fine"
    // The row the keys are on; the one asked about before removing or
    // signing out, and which of the two.
    property int focusIndex: 0
    property int confirming: -1
    property bool signingOut: false
    signal addRequested()
    signal signInRequested(string appleId)

    onShownChanged: {
        confirming = -1
        focusIndex = Math.max(0, sources.currentIndex)
    }

    function count(n) { return Number(n).toLocaleString(Qt.locale(), "f", 0) }
    function at(i) { return sources.list[i] }
    function open(i) {
        const s = at(i)
        if (!s)
            return
        if (s.id === sources.current && trouble(s) === "needs_sign_in")
            root.signInRequested(s.account)
        else
            sources.show(s.id)
    }
    // What is wrong with a source, as far as is known: only the one shown
    // is being looked after.
    function trouble(s) {
        if (s.id !== sources.current)
            return ""
        if (root.health === "needs_sign_in" && s.kind === "icloud")
            return "needs_sign_in"
        if (root.health === "offline" && s.kind === "folder")
            return "offline"
        return ""
    }
    function detail(s) {
        switch (trouble(s)) {
        case "needs_sign_in": return qsTr("Sign in again")
        case "offline": return qsTr("Not there: is its disk connected?")
        }
        const kind = s.kind === "icloud" ? "iCloud" : qsTr("Folder")
        return kind + " · " + count(s.count)
    }
    // When the one shown was last looked at for new photos, and when it is
    // next (the service's rounds), counted on as the bar is open; or
    // that Apple notifies of changes, when it does (a round then comes at
    // once, and the minute's rounds are only in case one is missed).
    property double now: Date.now()
    Timer {
        running: root.shown
        interval: 1000
        repeat: true
        triggeredOnStart: true
        onTriggered: root.now = Date.now()
    }
    function span(ms) {
        const s = Math.max(1, Math.round(ms / 1000))
        if (s < 60)
            return qsTr("%1 s").arg(s)
        if (s < 3600)
            return qsTr("%1 min").arg(Math.round(s / 60))
        return qsTr("%1 h").arg(Math.round(s / 3600))
    }
    // The two, a line each; nothing for a source not shown.
    function rounds(s) {
        const r = sources.rounds
        if (s.id !== sources.current || r.source !== s.id)
            return ""
        const ago = root.now - r.last
        const last = r.last === undefined ? qsTr("Not checked yet")
                   : ago < 5000 ? qsTr("Checked just now")
                   : qsTr("Checked %1 ago").arg(span(ago))
        const next = r.next === undefined || r.next <= root.now ? qsTr("Checking now…")
                   : r.push ? qsTr("Apple notifies of changes")
                   : qsTr("Next check in %1").arg(span(r.next - root.now))
        return last + "\n" + next
    }

    Shortcut { enabled: root.shown && root.confirming < 0; sequence: "Up"; context: Qt.ApplicationShortcut
        onActivated: root.focusIndex = Math.max(0, root.focusIndex - 1) }
    Shortcut { enabled: root.shown && root.confirming < 0; sequence: "Down"; context: Qt.ApplicationShortcut
        onActivated: root.focusIndex = Math.min(sources.list.length - 1, root.focusIndex + 1) }
    Shortcut {
        enabled: root.shown; sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut
        onActivated: {
            if (root.confirming >= 0 && root.signingOut) {
                sources.signOut(root.at(root.confirming).id)
                root.confirming = -1
            } else if (root.confirming >= 0) {
                sources.remove(root.at(root.confirming).id)
                root.confirming = -1
                root.focusIndex = Math.max(0, Math.min(root.focusIndex, sources.list.length - 2))
            } else {
                root.open(root.focusIndex)
            }
        }
    }
    Shortcut { enabled: root.shown && root.confirming < 0 && sources.list.length > 0; sequences: ["Delete", "Backspace"]; context: Qt.ApplicationShortcut
        onActivated: {
            root.signingOut = false
            root.confirming = root.focusIndex
        } }
    // Signing out of an Apple ID keeps its photos here; signed out, the row
    // offers signing in again instead.
    Shortcut {
        enabled: root.shown && root.confirming < 0 && root.at(root.focusIndex) !== undefined
                 && root.at(root.focusIndex).kind === "icloud" && root.trouble(root.at(root.focusIndex)) !== "needs_sign_in"
        sequence: "O"; context: Qt.ApplicationShortcut
        onActivated: {
            root.signingOut = true
            root.confirming = root.focusIndex
        }
    }
    Shortcut { enabled: root.shown && root.confirming < 0; sequence: "A"; context: Qt.ApplicationShortcut; onActivated: root.addRequested() }
    Shortcut {
        enabled: root.shown; sequence: "Escape"; context: Qt.ApplicationShortcut
        onActivated: root.confirming >= 0 ? root.confirming = -1 : root.shown = false
    }

    // The photos dimmed behind it, as behind Omarchy's menus and the other
    // panels (a scrim of the theme's background at half), coming and going
    // with the bar's slide: they are not what the keys work on meanwhile.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
        opacity: Math.max(0, Math.min(1, (panel.x + panel.width) / panel.width))
    }

    // A click beside it puts it away.
    MouseArea {
        anchors.fill: parent
        enabled: root.shown
        onClicked: root.shown = false
    }

    Rectangle {
        id: panel
        width: Math.min(parent.width - 48, theme.fontSize * 21)
        height: parent.height
        x: root.shown ? 0 : -width - 1
        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        color: Qt.rgba(theme.darkBackground.r, theme.darkBackground.g, theme.darkBackground.b, 0.96)
        MouseArea { anchors.fill: parent }
        Rectangle {
            anchors.right: parent.right
            width: 1
            height: parent.height
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.18)
        }

        Column {
            id: top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Math.round(theme.fontSize * 0.857)
            spacing: 6

            Item {
                width: parent.width
                height: Math.round(theme.fontSize * 2.333)
                UiText {
                    x: Math.round(theme.fontSize * 0.714)
                    y: centredIn(parent.height)
                    text: qsTr("Sources")
                    color: theme.accent
                }
                UiText {
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(theme.fontSize * 0.714)
                    y: centredIn(parent.height)
                    text: "Tab"
                    color: theme.darkForeground
                }
            }

            Repeater {
                model: sources.list
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool current: modelData.id === sources.current
                    readonly property bool asking: root.confirming === index
                    readonly property string trouble: root.trouble(modelData)
                    width: parent.width
                    height: rowContent.implicitHeight + Math.round(theme.fontSize * 1.143)
                    radius: Math.min(4, theme.radius)
                    color: current ? theme.selection
                         : hover.hovered ? Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.05) : "transparent"
                    border.width: 1
                    border.color: asking ? theme.red
                                : root.shown && root.focusIndex === index && sources.list.length > 1 ? theme.accent : "transparent"
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor; onHoveredChanged: if (hovered) root.focusIndex = row.index }
                    TapHandler { onTapped: root.confirming >= 0 ? root.confirming = -1 : root.open(row.index) }

                    Row {
                        id: rowContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Math.round(theme.fontSize * 0.714)
                        anchors.rightMargin: Math.round(theme.fontSize * 0.714)
                        spacing: Math.round(theme.fontSize * 0.714)
                        UiText {
                            id: glyph
                            width: Math.round(theme.fontSize * 1.429)
                            horizontalAlignment: Text.AlignHCenter
                            text: row.modelData.kind === "icloud" ? "" : ""
                            color: row.trouble !== "" ? theme.red : row.current ? theme.accent : theme.foreground
                        }
                        Column {
                            width: parent.width - glyph.width - number.width - 2 * parent.spacing
                            spacing: 2
                            UiText {
                                width: parent.width
                                text: row.modelData.name
                                color: row.trouble === "offline" ? theme.darkForeground : theme.brightForeground
                                wrapMode: Text.WrapAnywhere
                            }
                            UiText {
                                visible: !row.asking
                                width: parent.width
                                text: root.detail(row.modelData)
                                color: row.trouble !== "" ? theme.red : theme.darkForeground
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                            UiText {
                                visible: !row.asking && row.trouble === "" && text !== ""
                                width: parent.width
                                text: root.rounds(row.modelData)
                                wrapMode: Text.WordWrap
                                color: theme.darkForeground
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                            }
                            // Thumbnails still coming.
                            Rectangle {
                                visible: !row.asking && row.modelData.thumbs < row.modelData.count
                                width: parent.width
                                height: 2
                                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
                                Rectangle {
                                    width: parent.width * row.modelData.thumbs / Math.max(1, row.modelData.count)
                                    height: 2
                                    color: theme.accent
                                }
                            }
                            // Removing, asked first: what goes, and what stays.
                            UiText {
                                visible: row.asking
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: theme.red
                                font.pixelSize: Math.round(theme.fontSize * 0.857)
                                text: (root.signingOut
                                       ? qsTr("Sign out? Its index stays here, so signing in again is quick.")
                                       : row.modelData.kind === "icloud"
                                       ? qsTr("Sign out and forget its index? The photos stay in iCloud.")
                                       : qsTr("Forget its index? The photos stay in the folder."))
                                      + "  " + qsTr("⏎ yes · Esc no")
                            }
                        }
                        UiText {
                            id: number
                            text: row.index < 9 ? String(row.index + 1) : ""
                            color: theme.darkForeground
                        }
                    }
                }
            }

            ShellButton {
                width: parent.width
                text: qsTr("Add a source")
                hint: "A"
                onTapped: root.addRequested()
            }
        }

        // The keys, at the foot.
        Grid {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: Math.round(theme.fontSize * 1.571)
            columns: 2
            columnSpacing: theme.fontSize
            rowSpacing: 2
            // Key, words; key, words.
            Repeater {
                model: ["↑ ↓", qsTr("Choose"), "⏎", qsTr("Show it"), "1–9", qsTr("Show one, from anywhere"),
                        "O", qsTr("Sign out"), "Del", qsTr("Remove"), "Tab", qsTr("Hide")]
                delegate: UiText {
                    required property var modelData
                    required property int index
                    text: modelData
                    color: index % 2 === 0 ? theme.foreground : theme.darkForeground
                    font.pixelSize: Math.round(theme.fontSize * 0.857)
                }
            }
        }
    }
}
