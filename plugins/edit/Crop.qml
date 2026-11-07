import QtQuick
import Pinhole.Plugin

// Cropping the photo shown (K, Main.qml): the photo turned as it is, the
// part kept in a frame dragged by its corners and sides, or moved whole,
// what is left out dimmed; a shape to keep to (as the photo is, square,
// 4:3…), and R to turn it meanwhile. ⏎ keeps it, Esc leaves it as it was.
// The frame is in fractions of the photo turned, as pinhole.adjust takes
// it. `view` is its PluginView (Main.qml).
Item {
    id: crop
    objectName: "cropView"
    property string photo: ""
    property int turn: 0
    // The part kept: fractions of the photo as turned.
    property real bx: 0
    property real by: 0
    property real bw: 1
    property real bh: 1
    // Width over height to keep to, in the photo's pixels; 0 for any.
    property real shape: 0
    property string sharp: ""
    property bool fromOriginal: false
    readonly property var entry: photo !== "" ? (pinhole.library.revision, pinhole.library.item(photo)) : ({})

    // The photo's own size as its file is, then as turned.
    readonly property real rawW: picture.implicitWidth > 0 ? picture.implicitWidth : Math.max(1, entry.width || 1)
    readonly property real rawH: picture.implicitHeight > 0 ? picture.implicitHeight : Math.max(1, entry.height || 1)
    readonly property real turnedW: turn % 2 ? rawH : rawW
    readonly property real turnedH: turn % 2 ? rawW : rawH

    function start(id) {
        photo = id
        const now = pinhole.adjustment(id)
        turn = now.turn
        bx = now.crop.x; by = now.crop.y; bw = now.crop.width; bh = now.crop.height
        shape = 0
        sharp = ""
        // Cropped from the photo as taken: kept in iCloud, its thumbnail and
        // medium are of the edit, so the original (a HEIC, decoded as the
        // viewer decodes one); else the thumbnail at once and the medium.
        fromOriginal = now.kept === "source"
        if (fromOriginal)
            sharpening.start(() => pinhole.fetch(id, "original"), path => crop.sharp = "image://photo/" + encodeURIComponent(path))
        else
            sharpening.start(() => pinhole.fetch(id, "medium"), path => crop.sharp = "image://file/" + encodeURIComponent(path))
        Qt.callLater(() => crop.forceActiveFocus())
    }
    function stop() { sharpening.cancel() }
    function keep() {
        view.plugin.crop(photo, { turn: turn, crop: { x: bx, y: by, width: bw, height: bh } })
        view.close()
    }
    function whole() {
        bx = 0; by = 0; bw = 1; bh = 1
        if (shape > 0)
            keepTo(shape)
    }
    // A quarter more, the frame turning with the photo.
    function turnOnce() {
        const x = bx, y = by, w = bw, h = bh
        bx = 1 - y - h; by = x; bw = h; bh = w
        turn = (turn + 1) % 4
    }
    // The largest frame of that shape inside the one there, about its
    // middle, upright or lying as that one is.
    function keepTo(ratio) {
        shape = ratio
        if (ratio <= 0)
            return
        const wPx = bw * turnedW, hPx = bh * turnedH
        const r = (wPx >= hPx) === (ratio >= 1) ? ratio : 1 / ratio
        const w = Math.min(wPx, hPx * r), h = w / r
        const cx = bx + bw / 2, cy = by + bh / 2
        bw = w / turnedW; bh = h / turnedH
        bx = Math.min(Math.max(0, cx - bw / 2), 1 - bw)
        by = Math.min(Math.max(0, cy - bh / 2), 1 - bh)
    }
    Job { id: sharpening }

    // Over the photo and the viewer, taking every click and wheel.
    Panel {}

    Item {
        id: room
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: bar.top
        anchors.margins: theme.fontSize * 3
        readonly property real scale: Math.min(width / crop.turnedW, height / crop.turnedH)

        // The photo turned, as large as there is room.
        Item {
            id: sheet
            anchors.centerIn: parent
            width: crop.turnedW * room.scale
            height: crop.turnedH * room.scale
            Image {
                id: picture
                anchors.centerIn: parent
                width: crop.rawW * room.scale
                height: crop.rawH * room.scale
                rotation: crop.turn * 90
                source: crop.sharp !== "" ? crop.sharp : crop.fromOriginal ? "" : (crop.entry.thumbnailAsIs ?? "")
                fillMode: Image.Stretch
                asynchronous: true
                smooth: true
                mipmap: true
            }
            // What is left out, dimmed.
            readonly property rect box: Qt.rect(crop.bx * width, crop.by * height, crop.bw * width, crop.bh * height)
            Repeater {
                model: [Qt.rect(0, 0, sheet.width, sheet.box.y),
                        Qt.rect(0, sheet.box.y + sheet.box.height, sheet.width, sheet.height - sheet.box.y - sheet.box.height),
                        Qt.rect(0, sheet.box.y, sheet.box.x, sheet.box.height),
                        Qt.rect(sheet.box.x + sheet.box.width, sheet.box.y, sheet.width - sheet.box.x - sheet.box.width, sheet.box.height)]
                delegate: Rectangle {
                    required property rect modelData
                    x: modelData.x; y: modelData.y; width: modelData.width; height: modelData.height
                    color: Qt.rgba(0, 0, 0, 0.6)
                }
            }
            // The frame, in thirds, moved whole from inside.
            Item {
                id: frame
                x: sheet.box.x; y: sheet.box.y; width: sheet.box.width; height: sheet.box.height
                Rectangle { anchors.fill: parent; color: "transparent"; border.width: 2; border.color: "white" }
                Repeater {
                    model: 2
                    delegate: Item {
                        required property int index
                        anchors.fill: parent
                        Rectangle { x: frame.width * (index + 1) / 3; width: 1; height: frame.height; color: Qt.rgba(1, 1, 1, 0.45) }
                        Rectangle { y: frame.height * (index + 1) / 3; height: 1; width: frame.width; color: Qt.rgba(1, 1, 1, 0.45) }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    property point from
                    property real fx: 0
                    property real fy: 0
                    onPressed: mouse => { from = mapToItem(sheet, mouse.x, mouse.y); fx = crop.bx; fy = crop.by }
                    onPositionChanged: mouse => {
                        const at = mapToItem(sheet, mouse.x, mouse.y)
                        crop.bx = Math.min(Math.max(0, fx + (at.x - from.x) / sheet.width), 1 - crop.bw)
                        crop.by = Math.min(Math.max(0, fy + (at.y - from.y) / sheet.height), 1 - crop.bh)
                    }
                }
            }
            // Its corners and sides: [left, top, right, bottom] each moves.
            Repeater {
                model: [[1, 1, 0, 0], [0, 1, 1, 0], [0, 0, 1, 1], [1, 0, 0, 1],
                        [1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
                delegate: Rectangle {
                    id: grip
                    required property var modelData
                    required property int index
                    readonly property bool corner: index < 4
                    // Sides only while any shape will do.
                    visible: corner || crop.shape <= 0
                    width: corner ? 18 : (modelData[1] || modelData[3] ? 34 : 6)
                    height: corner ? 18 : (modelData[0] || modelData[2] ? 34 : 6)
                    radius: 2
                    color: "white"
                    x: (modelData[0] ? sheet.box.x : modelData[2] ? sheet.box.x + sheet.box.width : sheet.box.x + sheet.box.width / 2) - width / 2
                    y: (modelData[1] ? sheet.box.y : modelData[3] ? sheet.box.y + sheet.box.height : sheet.box.y + sheet.box.height / 2) - height / 2
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -10
                        cursorShape: grip.corner ? (grip.modelData[0] === grip.modelData[1] ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor)
                                                 : (grip.modelData[0] || grip.modelData[2] ? Qt.SizeHorCursor : Qt.SizeVerCursor)
                        onPositionChanged: mouse => {
                            const at = mapToItem(sheet, mouse.x, mouse.y)
                            const px = Math.min(Math.max(0, at.x / sheet.width), 1), py = Math.min(Math.max(0, at.y / sheet.height), 1)
                            const least = 0.05
                            let l = crop.bx, t = crop.by, r = crop.bx + crop.bw, b = crop.by + crop.bh
                            if (grip.modelData[0]) l = Math.min(px, r - least)
                            if (grip.modelData[2]) r = Math.max(px, l + least)
                            if (grip.modelData[1]) t = Math.min(py, b - least)
                            if (grip.modelData[3]) b = Math.max(py, t + least)
                            if (crop.shape > 0 && grip.corner) {
                                // Kept to its shape: as wide as dragged, as
                                // high as that makes it, from the corner held.
                                const ratio = (crop.bw * crop.turnedW >= crop.bh * crop.turnedH) === (crop.shape >= 1) ? crop.shape : 1 / crop.shape
                                let h = (r - l) * crop.turnedW / ratio / crop.turnedH
                                if (grip.modelData[1]) { h = Math.min(h, b); t = b - h } else { h = Math.min(h, 1 - t); b = t + h }
                                const w = h * crop.turnedH * ratio / crop.turnedW
                                if (grip.modelData[0]) l = r - w; else r = l + w
                            }
                            crop.bx = l; crop.by = t; crop.bw = r - l; crop.bh = b - t
                        }
                    }
                }
            }
        }
    }

    Row {
        id: bar
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: theme.fontSize * 1.5
        spacing: theme.fontSize
        Choices {
            id: shapes
            keyLayer: "plugin"
            when: view.shown
            section: qsTr("Crop")
            helpText: qsTr("Keep to a shape")
            value: crop.shape
            options: [{ value: 0, text: qsTr("Any") },
                      { value: crop.rawW / crop.rawH, text: qsTr("As taken") },
                      { value: 1, text: "1:1" }, { value: 4 / 3, text: "4:3" },
                      { value: 3 / 2, text: "3:2" }, { value: 16 / 9, text: "16:9" }]
            onChosen: value => crop.keepTo(value)
        }
        ShellButton { action: turnCommand; text: qsTr("Turn") }
        ShellButton { action: wholeCommand; text: qsTr("Whole photo") }
        ShellButton { action: view.closeCommand; text: qsTr("Cancel") }
        ShellButton { action: keepCommand; text: qsTr("Crop"); active: true }
    }
    Command {
        id: keepCommand
        name: "edit.keep"
        text: qsTr("Keep this crop")
        section: qsTr("Crop")
        keys: ["Return", "Enter"]
        keyLayer: "plugin"
        when: crop.visible
        onTriggered: crop.keep()
    }
    Command {
        id: turnCommand
        name: "edit.turn"
        text: qsTr("Turn it a quarter")
        section: qsTr("Crop")
        keys: ["R"]
        keyLayer: "plugin"
        when: crop.visible
        onTriggered: crop.turnOnce()
    }
    Command {
        id: wholeCommand
        name: "edit.whole"
        text: qsTr("The whole photo again")
        section: qsTr("Crop")
        keys: ["Backspace"]
        keyLayer: "plugin"
        when: crop.visible
        onTriggered: crop.whole()
    }
}
