import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import Pinhole

ApplicationWindow {
    id: win
    width: 1400
    height: 900
    visible: true
    title: "Pinhole"
    color: "#0e0e10"

    Material.theme: Material.Dark

    PhotoGrid {
        id: grid
        objectName: "grid"
        anchors.fill: parent
        focus: true
    }

    // Every vsync while anything moves; the benchmark drives it too.
    FrameAnimation {
        running: grid.animating || (bench !== null && bench.running)
        onTriggered: {
            if (bench !== null)
                bench.tick(frameTime)
            grid.advance(frameTime)
        }
    }

    Text {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        visible: grid.statsVisible
        text: grid.statsText
        color: "white"
        font.family: "monospace"
        font.pixelSize: 12
        style: Text.Outline
        styleColor: "black"
        horizontalAlignment: Text.AlignRight
    }

    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; onActivated: grid.statsVisible = !grid.statsVisible }
    Shortcut { sequences: ["+", "="]; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(-1) }
    Shortcut { sequence: "-"; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(1) }
    Shortcut { sequence: "Home"; context: Qt.ApplicationShortcut; onActivated: grid.scrollToStart() }
    Shortcut { sequence: "End"; context: Qt.ApplicationShortcut; onActivated: grid.scrollToEnd() }
    Shortcut { sequence: "PgUp"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(-1) }
    Shortcut { sequence: "PgDown"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(1) }
}
