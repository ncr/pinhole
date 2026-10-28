import QtQuick

// The keys, on ? or the ? button: as few words as will do, in as many
// columns as fit, scrolling only if the window is too short for them.
Item {
    id: root
    anchors.fill: parent
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    property bool shown: false

    readonly property var sections: [
        { title: qsTr("Library"), keys: [
            ["↑ ↓", qsTr("Scroll")],
            ["⇧ ↑ ↓", qsTr("Faster")],
            ["N", qsTr("Newest")],
            ["+ −", qsTr("Zoom")],
            ["Space", qsTr("Open")],
            ["S", qsTr("Select")],
            [qsTr("Drag"), qsTr("Select a range")]
        ] },
        { title: qsTr("Photo"), keys: [
            ["← →", qsTr("Previous, next")],
            ["Esc", qsTr("Back")],
            ["+ − 0", qsTr("Zoom")],
            ["L", qsTr("Live")],
            ["M", qsTr("Sound")]
        ] },
        { title: qsTr("Share"), keys: [
            ["C", qsTr("Copy")],
            ["E", qsTr("Export JPEG")],
            ["⇧ E", qsTr("Export original")],
            [qsTr("Drop files"), qsTr("Add to the library")]
        ] },
        { title: "Pinhole", keys: [
            ["Tab", qsTr("Sources")],
            ["1–9", qsTr("Show a source")],
            ["?", qsTr("Keys")],
            [",", qsTr("Settings")],
            ["Q", qsTr("Quit")]
        ] }
    ]

    // As Omarchy's menus: a scrim of the theme's background at half, and a
    // card of it with a hairline in the text colour.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.5)
    }
    MouseArea {
        anchors.fill: parent
        onClicked: root.shown = false
    }

    // A column as wide as its widest key and words, all columns alike.
    FontMetrics { id: metrics; font.family: theme.font; font.pixelSize: theme.fontSize }
    readonly property int gap: theme.fontSize
    function widest(texts) {
        let most = 0
        for (const text of texts)
            most = Math.max(most, metrics.advanceWidth(text))
        return Math.ceil(most)
    }
    readonly property int keyWidth: {
        const keys = []
        for (const section of sections)
            for (const key of section.keys)
                keys.push(key[0])
        return widest(keys)
    }
    readonly property int wordsWidth: {
        const words = []
        for (const section of sections) {
            words.push(section.title)
            for (const key of section.keys)
                words.push(key[1])
        }
        return widest(words)
    }
    readonly property int columnWidth: keyWidth + gap + wordsWidth
    readonly property int padding: 2 * theme.fontSize
    readonly property int columns: Math.max(1, Math.min(sections.length,
        Math.floor((width - 32 - 2 * padding + 2 * gap) / (columnWidth + 2 * gap))))

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, flow.width + 2 * root.padding)
        height: Math.min(parent.height - 32, flow.height + 2 * root.padding)
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.foreground
        border.width: 1
        clip: true

        Flickable {
            anchors.fill: parent
            anchors.margins: root.padding
            contentWidth: flow.width
            contentHeight: flow.height
            boundsBehavior: Flickable.StopAtBounds
            Flow {
                id: flow
                width: root.columns * root.columnWidth + (root.columns - 1) * 2 * root.gap
                spacing: 2 * root.gap
                Repeater {
                    model: root.sections
                    delegate: Column {
                        required property var modelData
                        width: root.columnWidth
                        spacing: Math.round(theme.fontSize * 0.4)
                        UiText {
                            x: root.keyWidth + root.gap
                            text: parent.modelData.title
                            color: theme.accent
                            bottomPadding: Math.round(theme.fontSize * 0.25)
                        }
                        Repeater {
                            model: parent.modelData.keys
                            delegate: Row {
                                required property var modelData
                                spacing: root.gap
                                UiText {
                                    width: root.keyWidth
                                    text: parent.modelData[0]
                                    color: theme.brightForeground
                                    horizontalAlignment: Text.AlignRight
                                }
                                UiText {
                                    text: parent.modelData[1]
                                    color: theme.darkForeground
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
