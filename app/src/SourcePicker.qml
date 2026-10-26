import QtQuick

// Where the photos come from, chosen: on the first run the whole window
// (nothing else to show yet), later a card from the sources bar's Add. What
// is already on this computer comes first, ready on one key (icloudpd's
// sign-in, folders of photos), then signing in to iCloud and choosing any
// folder.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false
    // The first run: the window's only content, with no way to close it.
    property bool firstRun: false
    // Signing in asked for: with icloudpd's session when `icloudpd`.
    signal signInRequested(bool icloudpd)

    onShownChanged: if (shown) sources.discover()

    function count(n) { return Number(n).toLocaleString(Qt.locale(), "f", 0) }

    // What is here already, best first, at most five.
    readonly property var suggestions: {
        const list = []
        if (sources.icloudpd.length > 0)
            list.push({ icloudpd: true, title: qsTr("iCloud Photos"),
                        detail: qsTr("icloudpd is signed in on this computer, so no password is needed") })
        // The biggest folders, past a few stray pictures when there is
        // more: three at most, a parent and its own subfolders alike.
        let folders = sources.folders.slice().sort((a, b) => b.assets - a.assets)
        if (folders.some(f => f.assets >= 20))
            folders = folders.filter(f => f.assets >= 20)
        for (const folder of folders.slice(0, 3)) {
            const parts = [qsTr("%Ln photo(s) and video(s)", "", folder.assets)]
            if (folder.live > 0)
                parts.push(qsTr("%Ln Live Photo(s)", "", folder.live))
            parts.push(storage.size(folder.bytes))
            list.push({ icloudpd: false, path: folder.path, title: folder.name, detail: parts.join(" · ") })
        }
        return list.slice(0, 5)
    }
    function use(i) {
        if (i >= suggestions.length)
            return
        const s = suggestions[i]
        if (s.icloudpd)
            root.signInRequested(true)
        else
            sources.addFolder(s.path)
    }

    Shortcut { enabled: root.shown; sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; onActivated: root.use(0) }
    Shortcut { enabled: root.shown && root.suggestions.length > 1; sequence: "2"; context: Qt.ApplicationShortcut; onActivated: root.use(1) }
    Shortcut { enabled: root.shown && root.suggestions.length > 2; sequence: "3"; context: Qt.ApplicationShortcut; onActivated: root.use(2) }
    Shortcut { enabled: root.shown && root.suggestions.length > 3; sequence: "4"; context: Qt.ApplicationShortcut; onActivated: root.use(3) }
    Shortcut { enabled: root.shown && root.suggestions.length > 4; sequence: "5"; context: Qt.ApplicationShortcut; onActivated: root.use(4) }
    Shortcut { enabled: root.shown; sequence: "I"; context: Qt.ApplicationShortcut; onActivated: root.signInRequested(false) }
    Shortcut { enabled: root.shown; sequence: "F"; context: Qt.ApplicationShortcut; onActivated: sources.chooseFolder() }
    Shortcut { enabled: root.shown && !root.firstRun; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: root.shown = false }

    // As the settings: a scrim, a card with a hairline. The first run has
    // neither, the window being all of it.
    Rectangle {
        anchors.fill: parent
        visible: !root.firstRun
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
    }
    MouseArea {
        anchors.fill: parent
        onClicked: if (!root.firstRun) root.shown = false
    }

    Rectangle {
        id: card
        readonly property int padding: root.firstRun ? 0 : 2 * theme.fontSize
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, theme.fontSize * 44 + 2 * padding)
        height: Math.min(parent.height - 32, content.implicitHeight + 2 * padding)
        radius: Math.min(4, theme.radius)
        color: root.firstRun ? "transparent" : theme.background
        border.color: theme.foreground
        border.width: root.firstRun ? 0 : 1
        clip: true
        MouseArea { anchors.fill: parent }

        Flickable {
            anchors.fill: parent
            anchors.margins: card.padding
            contentHeight: content.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: content
                width: parent.width
                spacing: 2 * theme.fontSize

                // The first run: what Pinhole is, in two lines.
                Column {
                    visible: root.firstRun
                    width: parent.width
                    spacing: theme.fontSize
                    Image {
                        anchors.horizontalCenter: parent.horizontalCenter
                        source: "qrc:/pinhole.svg"
                        sourceSize: Qt.size(width, height)
                        width: Math.round(theme.fontSize * 4)
                        height: width
                    }
                    UiText {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Where are your photos?")
                        color: theme.brightForeground
                        font.pixelSize: Math.round(theme.fontSize * 1.571)
                    }
                    UiText {
                        width: Math.min(parent.width, theme.fontSize * 39)
                        anchors.horizontalCenter: parent.horizontalCenter
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("Pinhole shows the whole library at once and fetches each photo only when you open it. It never changes anything in the library.")
                    }
                }

                Item {
                    visible: !root.firstRun
                    width: parent.width
                    height: close.height
                    UiText {
                        y: centredIn(parent.height)
                        text: qsTr("Add a source")
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

                // Found here: one key each, the first on Enter.
                Column {
                    visible: root.suggestions.length > 0
                    width: parent.width
                    spacing: 8
                    UiText {
                        text: qsTr("Found on this computer")
                        color: theme.darkForeground
                    }
                    Repeater {
                        model: root.suggestions
                        delegate: Rectangle {
                            id: row
                            required property var modelData
                            required property int index
                            readonly property bool first: index === 0
                            width: parent.width
                            height: rowContent.implicitHeight + 2 * Math.round(theme.fontSize)
                            radius: Math.min(4, theme.radius)
                            color: first ? Qt.rgba(Qt.color(theme.accent).r, Qt.color(theme.accent).g, Qt.color(theme.accent).b, 0.06)
                                 : hover.hovered ? Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.04) : "transparent"
                            border.width: 1
                            border.color: first ? theme.accent : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.18)
                            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.use(row.index) }

                            Row {
                                id: rowContent
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: theme.fontSize
                                anchors.rightMargin: theme.fontSize
                                spacing: theme.fontSize
                                UiText {
                                    id: glyph
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.round(theme.fontSize * 2)
                                    horizontalAlignment: Text.AlignHCenter
                                    // A cloud and a folder, from the Nerd Font Omarchy's type is.
                                    text: row.modelData.icloudpd ? "" : ""
                                    font.pixelSize: Math.round(theme.fontSize * 1.571)
                                    color: row.first ? theme.accent : theme.foreground
                                }
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - glyph.width - use.width - 2 * parent.spacing
                                    UiText {
                                        width: parent.width
                                        text: row.modelData.title
                                        color: theme.brightForeground
                                        wrapMode: Text.WordWrap
                                    }
                                    UiText {
                                        width: parent.width
                                        text: row.modelData.detail
                                        color: theme.darkForeground
                                        wrapMode: Text.WordWrap
                                    }
                                }
                                ShellButton {
                                    id: use
                                    anchors.verticalCenter: parent.verticalCenter
                                    active: row.first
                                    text: qsTr("Use")
                                    hint: row.first ? "⏎" : String(row.index + 1)
                                    onTapped: root.use(row.index)
                                }
                            }
                        }
                    }
                }

                // Anything else.
                Item {
                    width: parent.width
                    height: others.height
                    Row {
                        id: others
                        anchors.horizontalCenter: root.firstRun ? parent.horizontalCenter : undefined
                        spacing: 8
                        UiText {
                            visible: root.suggestions.length > 0
                            anchors.verticalCenter: parent.verticalCenter
                            rightPadding: 8
                            text: qsTr("Or")
                            color: theme.darkForeground
                        }
                        ShellButton {
                            text: qsTr("Sign in to iCloud")
                            hint: "I"
                            onTapped: root.signInRequested(false)
                        }
                        ShellButton {
                            text: qsTr("Choose a folder…")
                            hint: "F"
                            onTapped: sources.chooseFolder()
                        }
                    }
                }

                UiText {
                    visible: sources.error !== ""
                    width: parent.width
                    horizontalAlignment: root.firstRun ? Text.AlignHCenter : Text.AlignLeft
                    wrapMode: Text.WordWrap
                    color: theme.red
                    text: sources.error
                }
            }
        }
    }
}
