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
        onActivated: (index, rect) => viewer.openAt(index, rect)
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

    PhotoViewer {
        id: photoViewer
        anchors.fill: parent
        grid: grid
    }

    Text {
        anchors.bottom: parent.bottom
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

    readonly property bool inGrid: !photoViewer.shown

    function openCurrent() {
        const index = grid.currentIndex()
        if (index >= 0)
            viewer.openAt(index, grid.rectOf(index))
    }

    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; onActivated: grid.statsVisible = !grid.statsVisible }

    // The grid.
    Shortcut { enabled: win.inGrid; sequences: ["+", "="]; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(-1) }
    Shortcut { enabled: win.inGrid; sequence: "-"; context: Qt.ApplicationShortcut; onActivated: grid.zoomStep(1) }
    Shortcut { enabled: win.inGrid; sequence: "Home"; context: Qt.ApplicationShortcut; onActivated: grid.scrollToStart() }
    Shortcut { enabled: win.inGrid; sequence: "End"; context: Qt.ApplicationShortcut; onActivated: grid.scrollToEnd() }
    Shortcut { enabled: win.inGrid; sequence: "PgUp"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(-1) }
    Shortcut { enabled: win.inGrid; sequence: "PgDown"; context: Qt.ApplicationShortcut; onActivated: grid.scrollPages(1) }
    Shortcut { enabled: win.inGrid; sequences: ["Space", "Return"]; context: Qt.ApplicationShortcut; onActivated: win.openCurrent() }

    // The viewer.
    Shortcut { enabled: !win.inGrid; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: photoViewer.closeViewer() }
    Shortcut {
        enabled: !win.inGrid; sequences: ["Space", "Return"]; context: Qt.ApplicationShortcut
        // Space plays and pauses a video; on a photo it closes, like Quick Look.
        onActivated: if (!photoViewer.togglePlay()) photoViewer.closeViewer()
    }
    Shortcut { enabled: !win.inGrid; sequence: "Right"; context: Qt.ApplicationShortcut; onActivated: viewer.step(1) }
    Shortcut { enabled: !win.inGrid; sequence: "Left"; context: Qt.ApplicationShortcut; onActivated: viewer.step(-1) }
    Shortcut { enabled: !win.inGrid; sequence: "L"; context: Qt.ApplicationShortcut; onActivated: viewer.playLive() }
}
