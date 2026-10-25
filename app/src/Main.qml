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

    DateScrubber {
        grid: grid
        visible: win.inGrid
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

    // What is on its way out: "Preparing 3 photos…", "Exported 12 photos".
    Rectangle {
        visible: outgoing.status !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: statusText.implicitWidth + 24
        height: statusText.implicitHeight + 12
        radius: 6
        color: "#e0202024"
        Text {
            id: statusText
            anchors.centerIn: parent
            text: outgoing.status
            color: "white"
            font.pixelSize: 13
        }
    }

    readonly property bool inGrid: !photoViewer.shown

    // What Copy and Export act on: the selection, else the photo in view.
    function chosen() {
        if (grid.selectionCount > 0)
            return grid.selection()
        return viewer.open ? [viewer.index] : [grid.currentIndex()]
    }

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

    Shortcut { enabled: win.inGrid; sequence: StandardKey.SelectAll; context: Qt.ApplicationShortcut; onActivated: grid.selectAll() }
    Shortcut { enabled: win.inGrid && grid.selectionCount > 0; sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: grid.clearSelection() }

    // Out of the app, from the grid or the viewer.
    Shortcut { sequence: StandardKey.Copy; context: Qt.ApplicationShortcut; onActivated: outgoing.copy(win.chosen()) }
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; onActivated: outgoing.exportTo(win.chosen(), false) }
    Shortcut { sequence: "Ctrl+Shift+E"; context: Qt.ApplicationShortcut; onActivated: outgoing.exportTo(win.chosen(), true) }

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
