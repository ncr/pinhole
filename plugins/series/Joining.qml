import QtQuick
import Pinhole.Plugin

// The series a photo shown full window would join, while the pointer is on
// Add to series: a band along the foot of the window, over Pinhole's strip,
// with the series' photos as they would be, the joining one framed in the
// accent and the series' line under them all, as the strip draws one. A
// long series' thumbnails shrink to fit, down to a size still worth looking
// at; past that, the part around the joining photo shows, with how many
// more lie each way. A short one stays centred at full size.
Item {
    id: joining
    // Library positions in order, the joining one among them.
    property var shots: []
    property int joiner: -1
    property bool shown: false
    // The key that opens the series, said beside its name.
    property string key

    readonly property real most: 64
    readonly property real least: 28
    readonly property real gap: 4
    readonly property real margin: 16
    readonly property real room: Math.max(0, width - 2 * margin)
    // Slots that fit at the least size; one at an end may say how many more.
    readonly property int fits: Math.max(3, Math.floor((room + gap) / (least + gap)))
    // What shows, in order: {photo} for a photo, {more} for those cut off.
    readonly property var slots: {
        const n = shots.length, m = fits
        const at = Math.max(0, shots.indexOf(joiner))
        let from = 0, to = n
        if (n > m) {
            // Room for a count at each end, the joining photo in the middle;
            // at an end, that end's count gives its slot back.
            let k = m - 2
            from = Math.max(0, Math.min(n - k, at - Math.floor((k - 1) / 2)))
            if (from === 0)
                k = m - 1
            else if (from + k >= n)
                k = m - 1, from = n - k
            to = from + k
        }
        const list = []
        if (from > 0)
            list.push({ more: from })
        for (let i = from; i < to; ++i)
            list.push({ photo: shots[i] })
        if (to < n)
            list.push({ more: n - to })
        return list
    }
    readonly property real cell: slots.length === 0 ? most
        : Math.floor(Math.max(least, Math.min(most, (room + gap) / slots.length - gap)))

    height: column.height
    opacity: shown && shots.length > 0 ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    // The strip's own band: the theme's dark background, a hairline above.
    Rectangle {
        anchors.fill: parent
        color: theme.darkBackground
        Rectangle {
            width: parent.width
            height: 1
            color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.15)
        }
    }

    Column {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        topPadding: 10
        bottomPadding: 8
        spacing: 6
        UiText {
            anchors.horizontalCenter: parent.horizontalCenter
            // With its key, as a button shows one, quieter.
            color: joining.key !== "" ? theme.foreground : theme.darkForeground
            font.pixelSize: Math.round(theme.fontSize * 0.857)
            textFormat: Text.StyledText
            text: qsTr("Series of %1").arg(joining.shots.length)
                  + (joining.key !== "" ? " <font color=\"" + theme.darkForeground + "\">" + joining.key + "</font>" : "")
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: joining.gap
            Repeater {
                model: joining.slots
                delegate: Item {
                    id: slot
                    required property var modelData
                    required property int index
                    readonly property bool photo: modelData.photo !== undefined
                    readonly property bool joins: photo && modelData.photo === joining.joiner
                    width: joining.cell
                    height: joining.cell + 9
                    Image {
                        visible: slot.photo
                        width: joining.cell
                        height: joining.cell
                        source: joining.visible && slot.photo ? pinhole.library.item(slot.modelData.photo).thumbnail : ""
                        sourceSize: Qt.size(128, 128)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        // As the strip draws its thumbnails, and the joining
                        // one as it marks the one shown.
                        Rectangle {
                            anchors.fill: parent
                            color: "transparent"
                            border.width: slot.joins ? 2 : 1
                            border.color: slot.joins ? theme.accent
                                : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.28)
                        }
                    }
                    UiText {
                        visible: !slot.photo
                        width: joining.cell
                        height: joining.cell
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: theme.darkForeground
                        font.pixelSize: Math.round(Math.min(theme.fontSize, joining.cell * 0.4))
                        text: slot.photo ? "" : "+" + slot.modelData.more
                    }
                    // The series' line, across the gaps, rounded at its ends;
                    // dotted on where photos are cut off.
                    Rectangle {
                        readonly property bool first: slot.index === 0
                        readonly property bool last: slot.index === joining.slots.length - 1
                        visible: slot.photo
                        x: first ? 3 : 0
                        width: joining.cell - x + (last ? -3 : joining.gap)
                        y: joining.cell + 6
                        height: 3
                        radius: first || last ? 1.5 : 0
                        color: theme.accent
                    }
                    Rectangle {
                        visible: !slot.photo
                        x: slot.index === 0 ? 3 : 0
                        width: joining.cell - 3 + (slot.index === 0 ? joining.gap : 0)
                        y: joining.cell + 6
                        height: 3
                        radius: 1.5
                        color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.35)
                    }
                }
            }
        }
    }
}
