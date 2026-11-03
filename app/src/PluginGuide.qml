import QtQuick

// Instantiated only when requested: neither this view nor the Markdown
// documents are parsed on the startup path.
Panel {
    id: root
    objectName: "pluginGuide"
    signal closed()
    property string document: "plugin-quickstart"
    property int section: 0
    property string copied: ""
    readonly property bool wide: width >= 900
    readonly property var documents: [
        { id: "plugin-quickstart", title: qsTr("Start here") },
        { id: "plugins", title: qsTr("Plugin API") },
        { id: "source-providers", title: qsTr("Photo sources") },
        { id: "plugin-agents", title: qsTr("For agents") },
        { id: "service", title: qsTr("Service protocol") },
        { id: "sources", title: qsTr("Library schema") },
        { id: "hdr", title: qsTr("Colour and HDR") }
    ]
    readonly property var sections: pluginGuide.sections(document)
    readonly property var filtered: sections.map((s, i) => ({ title: s.title, id: s.id, index: i, markdown: s.markdown }))
        .filter(s => search.text === "" || s.markdown.toLocaleLowerCase().includes(search.text.toLocaleLowerCase()))
    readonly property var entry: filtered.length > 0 ? sections[section] : null
    function open(name, at) {
        search.text = ""
        document = name
        section = at ?? 0
        body.contentY = 0
    }
    function choose(at) { section = at; body.contentY = 0 }
    function step(direction) {
        const at = filtered.findIndex(s => s.index === section)
        if (filtered.length) choose(filtered[Math.max(0, Math.min(filtered.length - 1, at + direction))].index)
    }
    function copy(text, what) { pluginGuide.copy(text); copied = what; confirmation.restart() }
    function follow(link) {
        const target = pluginGuide.destination(link, document)
        if (target.document) open(target.document, target.section)
        else if (/^https?:\/\//.test(link)) Qt.openUrlExternally(link)
    }
    onFilteredChanged: if (filtered.length && !filtered.some(s => s.index === section)) choose(filtered[0].index)
    Timer { id: confirmation; interval: 2200; onTriggered: root.copied = "" }

    Command { name: "guide.close"; keyLayer: "guide"; keys: ["Escape"]; onTriggered: root.closed() }
    Command { name: "guide.search"; keyLayer: "guide"; keys: ["Ctrl+F"]; onTriggered: search.input.forceActiveFocus() }
    Command {
        name: "guide.previous"; keyLayer: "guide"; keys: ["Up"]
        when: !search.input.activeFocus && !reader.activeFocus
        autoRepeat: true; onTriggered: root.step(-1)
    }
    Command {
        name: "guide.next"; keyLayer: "guide"; keys: ["Down"]
        when: !search.input.activeFocus && !reader.activeFocus
        autoRepeat: true; onTriggered: root.step(1)
    }
    Command {
        name: "guide.nextDocument"; keyLayer: "guide"; keys: ["Ctrl+Tab"]
        onTriggered: root.open(root.documents[(root.documents.findIndex(d => d.id === root.document) + 1) % root.documents.length].id)
    }
    Command {
        name: "guide.pageDown"; keyLayer: "guide"; keys: ["PgDown"]
        autoRepeat: true; onTriggered: body.contentY = Math.min(Math.max(0, body.contentHeight - body.height), body.contentY + body.height * 0.85)
    }
    Command {
        name: "guide.pageUp"; keyLayer: "guide"; keys: ["PgUp"]
        autoRepeat: true; onTriggered: body.contentY = Math.max(0, body.contentY - body.height * 0.85)
    }

    readonly property int pad: Math.round(theme.fontSize * 1.5)
    Rectangle {
        id: frame
        anchors.centerIn: parent
        width: Math.min(1320, parent.width - 2 * Math.min(24, root.pad))
        height: Math.min(960, parent.height - 2 * Math.min(24, root.pad))
        color: theme.background
        border.width: 1
        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.2)
        radius: Math.min(4, theme.radius)

        Column {
            id: head
            x: root.pad; y: root.pad
            width: parent.width - 2 * x
            spacing: theme.fontSize
            Item {
                width: parent.width
                height: Math.max(title.implicitHeight, close.height)
                Column {
                    id: title
                    spacing: 4
                    UiText { text: qsTr("Plugin workshop"); font.pixelSize: Math.round(theme.fontSize * 1.5); font.weight: Font.DemiBold }
                    UiText { text: qsTr("API %1 · Built in · Available offline").arg(pluginGuide.api); color: theme.darkForeground; font.pixelSize: Math.round(theme.fontSize * 0.85) }
                }
                ShellButton { id: close; objectName: "guideClose"; anchors.right: parent.right; text: qsTr("Close"); hint: "Esc"; onTapped: root.closed() }
            }
            Flow {
                width: parent.width
                spacing: 6
                Repeater {
                    model: root.documents
                    delegate: ShellButton {
                        required property var modelData
                        required property int index
                        objectName: "guideTab-" + modelData.id
                        text: modelData.title
                        flat: true
                        active: root.document === modelData.id
                        key: String(index + 1)
                        keyLayer: "guide"
                        hint: ""
                        onTapped: root.open(modelData.id)
                    }
                }
            }
            Rectangle { width: parent.width; height: 1; color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12) }
        }

        Item {
            id: contents
            x: root.pad
            y: head.y + head.height + theme.fontSize
            width: parent.width - 2 * x
            height: parent.height - y - root.pad
            Column {
                id: navigation
                width: root.wide ? Math.round(theme.fontSize * 18) : parent.width
                spacing: theme.fontSize
                FieldInput {
                    id: search
                    objectName: "guideSearch"
                    width: parent.width
                    placeholder: qsTr("Find in this guide…")
                    onAccepted: {
                        search.input.focus = false
                        root.forceActiveFocus()
                    }
                }
                Flow {
                    id: tools
                    width: parent.width
                    spacing: 6
                    ShellButton {
                        objectName: "guideCopyAgent"
                        text: root.copied === "agent" ? qsTr("✓ Instructions copied") : qsTr("Copy agent brief")
                        onTapped: root.copy(pluginGuide.markdown("plugin-agents"), "agent")
                    }
                    ShellButton {
                        visible: !root.wide
                        text: "↑"; onTapped: root.step(-1)
                    }
                    ShellButton {
                        visible: !root.wide
                        text: "↓"; onTapped: root.step(1)
                    }
                }
            }

            ScrollList {
                id: toc
                objectName: "guideSections"
                visible: root.wide
                y: navigation.height + theme.fontSize
                width: navigation.width
                height: parent.height - y
                model: root.filtered
                spacing: 4
                currentIndex: root.filtered.findIndex(s => s.index === root.section)
                onCurrentIndexChanged: positionViewAtIndex(Math.max(0, currentIndex), ListView.Contain)
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    width: toc.width
                    height: label.implicitHeight + theme.fontSize * 1.2
                    radius: Math.min(4, theme.radius)
                    color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b,
                                  press.pressed ? 0.22 : hover.hovered ? 0.08 : root.section === modelData.index ? 0.12 : 0)
                    border.width: 1
                    border.color: root.section === modelData.index ? Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.25) : "transparent"
                    UiText {
                        id: label
                        x: theme.fontSize * 0.7
                        y: theme.fontSize * 0.6
                        width: parent.width - 2 * x
                        wrapMode: Text.WordWrap
                        text: row.modelData.title
                        color: root.section === row.modelData.index ? theme.foreground : theme.darkForeground
                    }
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { id: press; onTapped: root.choose(row.modelData.index) }
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.title
                    Accessible.onPressAction: root.choose(modelData.index)
                }
            }

            Rectangle {
                id: reading
                x: root.wide ? navigation.width + root.pad : 0
                y: root.wide ? 0 : navigation.height + theme.fontSize
                width: parent.width - x
                height: parent.height - y
                color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.025)
                border.width: 1
                border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.1)
                radius: Math.min(4, theme.radius)
                Item {
                    id: readingHead
                    x: root.pad; y: theme.fontSize * 0.7
                    width: parent.width - 2 * x
                    height: sectionCopy.height
                    UiText {
                        width: parent.width - sectionCopy.width - theme.fontSize
                        y: centredIn(parent.height)
                        text: root.entry ? (root.section + 1) + " / " + root.sections.length + " · " + root.entry.title : qsTr("No matching section")
                        elide: Text.ElideRight
                        color: theme.darkForeground
                        font.pixelSize: Math.round(theme.fontSize * 0.85)
                    }
                    ShellButton {
                        id: sectionCopy
                        objectName: "guideCopySection"
                        anchors.right: parent.right
                        text: root.copied === "section" ? qsTr("✓ Copied") : qsTr("Copy section")
                        usable: root.entry !== null
                        flat: true
                        onTapped: root.copy(root.entry.markdown, "section")
                    }
                }
                ScrollArea {
                    id: body
                    objectName: "guideBody"
                    x: root.pad
                    y: readingHead.y + readingHead.height + theme.fontSize
                    width: parent.width - 2 * x
                    height: parent.height - y - root.pad
                    contentWidth: width
                    contentHeight: article.height
                    Column {
                        id: article
                        width: parent.width
                        spacing: root.pad
                        Column {
                            visible: root.document === "plugin-quickstart" && root.section === 0 && root.entry !== null
                            width: parent.width
                            spacing: theme.fontSize
                            UiText { text: qsTr("YOUR PHOTOS. YOUR IDEAS."); color: theme.accent; font.pixelSize: Math.round(theme.fontSize * 0.85); font.letterSpacing: 1.5 }
                            UiText {
                                width: parent.width
                                text: qsTr("A small plugin.\nA whole new possibility.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: Math.round(theme.fontSize * 2)
                                font.weight: Font.DemiBold
                            }
                            UiText {
                                width: parent.width
                                text: qsTr("A print studio, a birthday wall, a new photo source. Make Pinhole fit the way you work — save a file and see it change.")
                                wrapMode: Text.WordWrap
                                color: theme.darkForeground
                            }
                            Flow {
                                width: parent.width
                                spacing: 8
                                Repeater {
                                    model: [qsTr("Views and workflows"), qsTr("Tools and automation"), qsTr("New photo sources")]
                                    delegate: Chip { required property string modelData; text: modelData; textColor: theme.accent }
                                }
                            }
                            Rectangle { width: parent.width; height: 1; color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12) }
                        }
                        TextEdit {
                            id: reader
                            objectName: "guideReader"
                            width: parent.width
                            text: root.entry ? root.entry.markdown : qsTr("Try another word, or clear the search to show every section.")
                            textFormat: TextEdit.MarkdownText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: theme.foreground
                            selectionColor: theme.selection
                            selectedTextColor: theme.brightForeground
                            font.family: theme.font
                            font.pixelSize: theme.fontSize
                            onLinkActivated: link => root.follow(link)
                            onCursorRectangleChanged: {
                                if (!activeFocus) return
                                const y = mapToItem(article, 0, cursorRectangle.y).y
                                if (y < body.contentY) body.contentY = y
                                else if (y + cursorRectangle.height > body.contentY + body.height)
                                    body.contentY = y + cursorRectangle.height - body.height
                            }
                        }
                        UiText {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: qsTr("The reference and code examples are in English. The app and CLI share these exact documents.")
                            font.pixelSize: Math.round(theme.fontSize * 0.85)
                            color: theme.darkForeground
                        }
                    }
                }
            }
        }
    }
}
