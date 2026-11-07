import QtQuick
import Pinhole
import "focus.js" as Focus

// Where the photos come from, chosen: on the first run the whole window
// (nothing else to show yet), later a card from the sources bar's Add. What
// is already on this computer comes first, ready on one key (icloudpd's
// sign-in, folders of photos), then the ways in that are always there
// (signing in to iCloud, choosing any folder): each of them a source
// plugin's (PluginSource.offers and .ways).
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false
    // The first run: the window's only content, with no way to close it.
    property bool firstRun: false
    // The kinds of source the plugins on bring (PluginSource), in the
    // service's order: iCloud's first.
    property var kinds: []
    property var returnFocus: null
    Accessible.role: Accessible.Dialog
    Accessible.name: firstRun ? qsTr("Where are your photos?") : qsTr("Add a source")
    Accessible.ignored: !shown
    FocusFence.enabled: shown
    function currentFocus() { return root.Window.window ? root.Window.window.activeFocusItem : null }
    function restoreFocus() {
        const all = Focus.items(root)
        const first = all.find(item => item !== close) ?? all[0]
        if (first) first.forceActiveFocus(Qt.TabFocusReason)
        else root.forceActiveFocus()
    }
    onShownChanged: {
        if (shown) {
            returnFocus = currentFocus()
            sources.discover()
            Qt.callLater(restoreFocus)
        } else Qt.callLater(() => {
            if (["grid", "side"].includes(keymap.layer) && returnFocus?.visible && returnFocus.enabled)
                returnFocus.forceActiveFocus()
        })
    }
    Keys.onShortcutOverride: event => {
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) event.accepted = true
    }
    Keys.onPressed: event => {
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
            Focus.move(root, event.key === Qt.Key_Backtab || (event.modifiers & Qt.ShiftModifier) ? -1 : 1, currentFocus())
            event.accepted = true
        }
    }

    // What is here already, best first, at most five: each plugin's offers
    // in turn ({ title, detail, icon, triggered }).
    readonly property var suggestions: {
        let list = []
        for (const kind of kinds)
            for (const offer of kind.offers || [])
                list.push(Object.assign({ icon: kind.icon }, offer))
        return list.slice(0, 5)
    }
    // The ways in that are always there: { title, key, triggered }.
    readonly property var ways: {
        let list = []
        for (const kind of kinds)
            list = list.concat(kind.ways || [])
        return list
    }
    function use(i) {
        if (i < suggestions.length)
            suggestions[i].triggered()
    }

    // Its keys, while it is on top (Command.qml): the first run's, or adding
    // another source's.
    readonly property string keyLayer: firstRun ? "firstRun" : "addSource"
    Repeater {
        model: 5
        delegate: Command {
            required property int index
            name: "pick.use" + (index + 1)
            keys: index === 0 ? ["Return", "Enter"] : [String(index + 1)]
            keyLayer: root.keyLayer
            usable: root.shown && index < root.suggestions.length
            mouse: qsTr("Its Use button")
            touchpad: qsTr("Its Use button")
            onTriggered: root.use(index)
        }
    }
    Command {
        id: closeAction
        name: "pick.close"
        keys: ["Escape"]
        keyLayer: root.keyLayer
        usable: root.shown && !root.firstRun
        onTriggered: root.shown = false
    }

    // As the settings: a scrim, a card with a hairline. The first run has
    // neither, the window being all of it.
    Rectangle {
        anchors.fill: parent
        visible: !root.firstRun
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
    }
    // A click beside it puts it away; nothing there reaches the photos.
    Shield {
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
        Shield { anchors.fill: parent }

        Flickable {
            id: scroller
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
                        action: closeAction
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
                            Accessible.role: Accessible.Grouping
                            Accessible.name: modelData.title
                            Accessible.description: modelData.detail
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
                                    // A cloud, a folder: the Nerd Font Omarchy's type is.
                                    text: row.modelData.icon
                                    font.pixelSize: Math.round(theme.fontSize * 1.571)
                                    color: row.first ? theme.accent : theme.foreground
                                    Accessible.ignored: true
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
                                    objectName: "sourceUse-" + row.index
                                    Accessible.name: qsTr("Use %1").arg(row.modelData.title)
                                    anchors.verticalCenter: parent.verticalCenter
                                    active: row.first
                                    text: qsTr("Use")
                                    hint: row.first ? "⏎" : String(row.index + 1)
                                    onTriggered: root.use(row.index)
                                }
                            }
                        }
                    }
                }

                // Anything else.
                Flow {
                    width: parent.width
                    spacing: 8
                    UiText {
                        visible: root.suggestions.length > 0
                        height: Math.round(theme.fontSize * 2.333)
                        verticalAlignment: Text.AlignVCenter
                        rightPadding: 8
                        text: qsTr("Or")
                        color: theme.darkForeground
                    }
                    Repeater {
                        model: root.ways
                        delegate: ShellButton {
                            required property var modelData
                            text: modelData.title
                            key: modelData.key ?? ""
                            keyLayer: root.keyLayer
                            usable: root.shown
                            onTriggered: modelData.triggered()
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
        Scroller {
            anchors.fill: parent
            flickable: scroller
        }
    }
}
